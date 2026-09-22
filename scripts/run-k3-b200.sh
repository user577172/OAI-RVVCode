#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(realpath "$(dirname "${BASH_SOURCE[0]}")")
OAI_ROOT="${OAI_ROOT:-$(realpath "$SCRIPT_DIR/..")}" 
LEGACY_ROOT="${SIONNA_RK_ROOT:-$(realpath "$OAI_ROOT/../..")}" 
B210_CONFIG_DIR="${B210_CONFIG_DIR:-$OAI_ROOT/b210}"
ENV_FILE="${B210_ENV_FILE:-${B200_ENV_FILE:-$B210_CONFIG_DIR/.env}}"
DEFAULT_BUILD="$OAI_ROOT/cmake_targets/ran_build/build"
BUILD="$DEFAULT_BUILD"
GNB_BIN="$BUILD/nr-softmodem"
GNB_TEMPLATE="${GNB_TEMPLATE:-$B210_CONFIG_DIR/gnb.sa.band78.24prbs.conf}"
RUNTIME_DIR="${RUNTIME_DIR:-$OAI_ROOT/.runtime/k3-b200}"
RUNTIME_CONF="$RUNTIME_DIR/gnb.conf"
UNIT="k3-b200-gnb.service"
RESULTS_DIR="${RESULTS_DIR:-$OAI_ROOT/results}"
OPT_LOG="$RESULTS_DIR/k3-b200-optimization.log"
JOURNAL_LOG="$RESULTS_DIR/k3-b200-gnb.log"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
active() { sudo systemctl is-active --quiet "$UNIT"; }

ensure_core() {
    if ! pgrep -x upf >/dev/null || ! pgrep -x smf >/dev/null || ! pgrep -x amf >/dev/null; then
        if [ -x "$LEGACY_ROOT/scripts/start-cn5g-k3.sh" ]; then
            "$LEGACY_ROOT/scripts/start-cn5g-k3.sh"
        else
            die "CN5G is not running and no core-network launcher was found. Start AMF/SMF/UPF first."
        fi
    fi
}

ensure_ue_internet() {
    if command -v k3-ue-internet >/dev/null 2>&1; then
        sudo k3-ue-internet apply
        return
    fi
    local uplink="${K3_UPLINK_IF:-}"
    if [ -z "$uplink" ]; then
        uplink=$(ip -4 route show default | awk 'NR == 1 { print $5 }')
    fi
    [ -n "$uplink" ] || die "No default-route interface found; set K3_UPLINK_IF."
    ip link show "$uplink" >/dev/null 2>&1 || die "Uplink interface does not exist: $uplink"

    sudo sysctl -q -w net.ipv4.ip_forward=1
    sudo iptables -C FORWARD -i tun0 -o "$uplink" -j ACCEPT 2>/dev/null \
        || sudo iptables -A FORWARD -i tun0 -o "$uplink" -j ACCEPT
    sudo iptables -C FORWARD -i "$uplink" -o tun0 \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null \
        || sudo iptables -A FORWARD -i "$uplink" -o tun0 \
            -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    sudo iptables -t nat -C POSTROUTING -s 12.1.1.0/24 -o "$uplink" -j MASQUERADE 2>/dev/null \
        || sudo iptables -t nat -A POSTROUTING -s 12.1.1.0/24 -o "$uplink" -j MASQUERADE
    record "ue-internet uplink=$uplink subnet=12.1.1.0/24 forwarding=enabled"
}

record() {
    mkdir -p "$RESULTS_DIR"
    printf '%s %s\n' "$(date --iso-8601=seconds)" "$*" | tee -a "$OPT_LOG"
}

apply_host_tuning() {
    # Keep the B210 xHCI interrupt away from the eight A100 PHY cores. Retain
    # the kernel's RT safety budget so management/network tasks cannot starve.
    sudo sysctl -q -w kernel.sched_rt_runtime_us=950000
    local irq
    irq=$(awk '/xhci-hcd:usb5$/ {gsub(":", "", $1); print $1; exit}' /proc/interrupts)
    if [ -n "$irq" ] && [ -e "/proc/irq/$irq/smp_affinity" ]; then
        echo 80 | sudo tee "/proc/irq/$irq/smp_affinity" >/dev/null
        record "host-tuning rt_runtime_us=950000 b210_xhci_irq=$irq irq_affinity=cpu7"
    else
        record "host-tuning rt_runtime_us=950000 b210_xhci_irq=not-found"
    fi
}

spread_uhd_ru_workers() {
    # ru_thread_core is intentionally kept on CPU 2 for the main RU thread,
    # but UHD helper threads inherit that one-CPU mask.  On K3 this made three
    # busy SCHED_RR/97 threads compete on CPU 2 and caused slot-scale wakeup
    # delays.  Identify the two busiest helper threads after startup and move
    # them to scalar CPUs 3 and 4.  Failure is non-fatal and is recorded.
    local pid task tid ticks delta i=0
    local -a ru_tids=() cores=(3 4)
    local -A before=()

    pid=$(systemctl show "$UNIT" -p MainPID --value)
    [[ $pid =~ ^[1-9][0-9]*$ && -d /proc/$pid/task ]] || {
        record "ru-worker-spread skipped reason=no-main-pid"
        return 0
    }
    while IFS= read -r tid; do ru_tids+=("$tid"); done < <(
        for task in /proc/"$pid"/task/*; do
            [ "$(<"$task/comm")" = ru_thread ] && printf '%s\n' "${task##*/}"
        done | sort -n
    )
    ((${#ru_tids[@]} >= 3)) || {
        record "ru-worker-spread skipped reason=only-${#ru_tids[@]}-ru-threads"
        return 0
    }

    # Keep the oldest/main RU thread on CPU 2 and rank only its helpers.
    for tid in "${ru_tids[@]:1}"; do
        before[$tid]=$(awk '{print $14+$15}' "/proc/$pid/task/$tid/stat" 2>/dev/null || echo 0)
    done
    sleep 1
    while read -r delta tid; do
        [ -n "${tid:-}" ] || continue
        if sudo taskset -pc "${cores[$i]}" "$tid" >/dev/null; then
            record "ru-worker-spread tid=$tid cpu=${cores[$i]} tick_delta=$delta main_ru_tid=${ru_tids[0]}"
        else
            record "ru-worker-spread failed tid=$tid cpu=${cores[$i]}"
        fi
        ((++i >= ${#cores[@]})) && break
    done < <(
        for tid in "${ru_tids[@]:1}"; do
            ticks=$(awk '{print $14+$15}' "/proc/$pid/task/$tid/stat" 2>/dev/null || echo 0)
            delta=$((ticks - before[$tid]))
            printf '%s %s\n' "$delta" "$tid"
        done | sort -k1,1nr
    )
}

load_env() {
    [ -r "$ENV_FILE" ] || die "Missing $ENV_FILE"
    set -a
    set +u
    # This is the project-owned shell-style environment file used by Compose.
    # shellcheck disable=SC1090
    source "$ENV_FILE"
    set -u
    set +a
}

valid_ipv4() {
    local ip=$1 part
    [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || return 1
    IFS=. read -r -a parts <<< "$ip"
    for part in "${parts[@]}"; do
        (( part >= 0 && part <= 255 )) || return 1
    done
}

select_gnb_ip() {
    if [ -n "${GNB_IP:-}" ]; then
        printf '%s\n' "$GNB_IP"
        return
    fi
    ip -4 route get "$AMF_IP" 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}'
}

preflight() {
    load_env
    # K3_GNB_BUILD is defined by the project .env, so resolve it only after
    # load_env. This keeps the original build as the automatic fallback.
    BUILD="${K3_GNB_BUILD:-$DEFAULT_BUILD}"
    GNB_BIN="$BUILD/nr-softmodem"
    [ "$(uname -m)" = "riscv64" ] || die "This launcher is for the RV64 K3; detected $(uname -m)."
    [ -x "$GNB_BIN" ] || die "Missing $GNB_BIN; build the native OAI gNB first."
    [ -r "$GNB_TEMPLATE" ] || die "Missing gNB template: $GNB_TEMPLATE"
    command -v uhd_find_devices >/dev/null || die "uhd_find_devices is missing; install/build UHD for riscv64."
    command -v uhd_usrp_probe >/dev/null || die "uhd_usrp_probe is missing; install/build UHD for riscv64."
    [ -n "${USRP_SERIAL:-}" ] || die "Set USRP_SERIAL in $ENV_FILE"
    [ -n "${AMF_IP:-}" ] || die "Set AMF_IP in $ENV_FILE to the external 5G Core AMF address."
    valid_ipv4 "$AMF_IP" || die "AMF_IP is not a valid IPv4 address: $AMF_IP"
    RESOLVED_GNB_IP=$(select_gnb_ip)
    [ -n "$RESOLVED_GNB_IP" ] || die "Could not select a K3 source address for AMF $AMF_IP; set GNB_IP explicitly."
    valid_ipv4 "$RESOLVED_GNB_IP" || die "GNB_IP is not a valid IPv4 address: $RESOLVED_GNB_IP"
    ip -4 addr show | grep -qw "$RESOLVED_GNB_IP" || die "GNB_IP $RESOLVED_GNB_IP is not assigned to this K3."
    uhd_find_devices 2>&1 | grep -Fq "$USRP_SERIAL" || die "USRP $USRP_SERIAL was not found by UHD."
}

write_runtime_config() {
    mkdir -p "$RUNTIME_DIR"
    cp "$GNB_TEMPLATE" "$RUNTIME_CONF"
    sed -Ei \
        -e 's#tracking_area_code[[:space:]]*=[[:space:]]*[^;]+;#tracking_area_code  = 1;#' \
        -e 's#plmn_list[[:space:]]*=[[:space:]]*\(\{[[:space:]]*mcc[[:space:]]*=[[:space:]]*[0-9]+;[[:space:]]*mnc[[:space:]]*=[[:space:]]*[0-9]+;[[:space:]]*mnc_length[[:space:]]*=[[:space:]]*[0-9]+;#plmn_list = ({ mcc = 262; mnc = 99; mnc_length = 2;#' \
        -e "s#(amf_ip_address[[:space:]]*=[[:space:]]*\\(\\{[[:space:]]*ipv4[[:space:]]*=[[:space:]]*)\"[^\"]+\"#\\1\"$AMF_IP\"#" \
        -e "s#(GNB_IPV4_ADDRESS_FOR_NG_AMF[[:space:]]*=[[:space:]]*)\"[^\"]+\"#\\1\"$RESOLVED_GNB_IP\"#" \
        -e "s#(GNB_IPV4_ADDRESS_FOR_NGU[[:space:]]*=[[:space:]]*)\"[^\"]+\"#\\1\"$RESOLVED_GNB_IP\"#" \
        "$RUNTIME_CONF"
    grep -Fq "ipv4 = \"$AMF_IP\"" "$RUNTIME_CONF" || die "Failed to write AMF_IP to runtime config."
    [ "$(grep -Fc "\"$RESOLVED_GNB_IP\"" "$RUNTIME_CONF")" -ge 2 ] || die "Failed to write GNB_IP to runtime config."
}

start_gnb() {
    ensure_core
    ensure_ue_internet
    preflight
    local allowed_cpus="${K3_GNB_ALLOWED_CPUS:-8-15}"
    active && die "$UNIT is already active."
    pgrep -x nr-softmodem >/dev/null && die "An unmanaged nr-softmodem process is already running."
    write_runtime_config
    apply_host_tuning
    record "start kernel=$(uname -r) build=$BUILD usrp=$USRP_SERIAL amf=$AMF_IP gnb_ip=$RESOLVED_GNB_IP thread_pool=${K3_GNB_THREAD_POOL:-default} l1_rx=-1 l1_tx=-1 ru_pool=-1x5 ru_thread=-1 allowed_cpus=$allowed_cpus"

    args=(
        "$GNB_BIN" -O "$RUNTIME_CONF"
        --RUs.[0].sdr_addrs "serial=$USRP_SERIAL"
        --telnetsrv
        --reorder-thread-disable 1
        --log_config.global_log_options level,nocolor,time
    )
    # Continuous TX can occupy the small 24-PRB downlink grid and leave no
    # VRB/CCE for Msg4 during real-UE random access. Keep it opt-in for RF
    # diagnostics instead of enabling it for normal UE operation.
    if [ "${K3_GNB_CONTINUOUS_TX:-0}" = 1 ]; then
        args+=(--continuous-tx)
    fi
    if [ -n "${K3_GNB_THREAD_POOL:-}" ]; then
        args+=(--thread-pool "$K3_GNB_THREAD_POOL")
    fi
    if [ -n "${GNB_EXTRA_OPTIONS:-}" ]; then
        read -r -a extra <<< "$GNB_EXTRA_OPTIONS"
        args+=("${extra[@]}")
    fi

    sudo systemctl reset-failed "$UNIT" 2>/dev/null || true
    sudo systemd-run \
        --unit="$UNIT" \
        --service-type=exec \
        --property=Restart=no \
        --property=KillMode=mixed \
        --property=TimeoutStopSec=10 \
        --property="AllowedCPUs=$allowed_cpus" \
        --property=LimitMEMLOCK=infinity \
        --property=Nice=-20 \
        --property=TasksMax=infinity \
        --setenv="LD_LIBRARY_PATH=$BUILD" \
        --working-directory="$BUILD" \
        "${args[@]}"
    sleep 3
    active || {
        sudo journalctl -u "$UNIT" -n 120 --no-pager
        die "Native B200 gNB failed to start."
    }
    spread_uhd_ru_workers
    echo "Native K3 B200 gNB started."
    echo "AMF: $AMF_IP; K3 N2/N3 address: $RESOLVED_GNB_IP; USRP: $USRP_SERIAL"
    echo "Logs: $0 log"
}

case "${1:-}" in
    check)
        ensure_core
        preflight
        write_runtime_config
        echo "Preflight passed. Runtime config: $RUNTIME_CONF"
        ;;
    start) start_gnb ;;
    status)
        if [ -x "$LEGACY_ROOT/scripts/status-cn5g-k3.sh" ]; then
            "$LEGACY_ROOT/scripts/status-cn5g-k3.sh"
        else
            for process in amf smf upf; do
                if pgrep -x "$process" >/dev/null; then
                    echo "$process: running"
                else
                    echo "$process: not running"
                fi
            done
        fi
        sudo systemctl --no-pager --full status "$UNIT" || true
        sudo journalctl -u "$UNIT" --no-pager | tee "$JOURNAL_LOG" | grep -E 'Received NGSetupResponse|associated AMF|No UHD Devices|RuntimeError|ERROR' | tail -20 || true
        ;;
    log)
        mkdir -p "$RESULTS_DIR"
        sudo journalctl -fu "$UNIT" -n 100 | tee -a "$JOURNAL_LOG"
        ;;
    stop)
        sudo systemctl stop "$UNIT" 2>/dev/null || true
        mkdir -p "$RESULTS_DIR"
        sudo journalctl -u "$UNIT" --no-pager >"$JOURNAL_LOG" || true
        if [ -x "$LEGACY_ROOT/scripts/stop-cn5g-k3.sh" ]; then
            "$LEGACY_ROOT/scripts/stop-cn5g-k3.sh"
            echo "Native K3 B200 gNB and CN5G stopped."
        else
            echo "Native K3 B200 gNB stopped; external CN5G was left running."
        fi
        ;;
    *)
        echo "Usage: $0 check|start|status|log|stop"
        exit 2
        ;;
esac
