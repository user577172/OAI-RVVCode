#!/usr/bin/env bash
# K3 RFsim hotspot experiment launcher (systemd-isolated revision).
set -euo pipefail

SCRIPT_DIR=$(realpath "$(dirname "${BASH_SOURCE[0]}")")
OAI="${OAI_ROOT:-$(realpath "$SCRIPT_DIR/..")}" 
BUILD="$OAI/cmake_targets/ran_build/build"
RESULTS_DIR="${RESULTS_DIR:-$OAI/results}"
GNB_IDEAL="$OAI/ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf"
GNB_AWGN="${GNB_AWGN:-$OAI/.runtime/rfsim/gnb.sa.band78.106prb.awgn.conf}"
UE_CONF="$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.conf"
CPU_SET="${CPU_SET:-0-7}"
MEASURE_SECONDS="${MEASURE_SECONDS:-120}"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
COOLDOWN_SECONDS="${COOLDOWN_SECONDS:-30}"
SYNC_TIMEOUT="${SYNC_TIMEOUT:-20}"
SYNC_ATTEMPTS="${SYNC_ATTEMPTS:-3}"

MODE="${1:-}"
REPEATS="${2:-1}"
START_RUN="${3:-1}"

usage() {
  echo "Usage: $0 ideal|awgn|pair [repeat-count] [start-number]"
  echo "Example: $0 pair 5"
}

die() { echo "ERROR: $*" >&2; exit 1; }

[[ "$MODE" =~ ^(ideal|awgn|pair)$ ]] || { usage; exit 1; }
[[ "$REPEATS" =~ ^[1-9][0-9]*$ ]] || die "repeat-count must be a positive integer"
[[ "$START_RUN" =~ ^[1-9][0-9]*$ ]] || die "start-number must be a positive integer"
[[ "$SYNC_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || die "SYNC_TIMEOUT must be a positive integer"
[[ "$SYNC_ATTEMPTS" =~ ^[1-9][0-9]*$ ]] || die "SYNC_ATTEMPTS must be a positive integer"
[[ -x "$BUILD/nr-softmodem" ]] || die "nr-softmodem not found: $BUILD"
[[ -x "$BUILD/nr-uesoftmodem" ]] || die "nr-uesoftmodem not found: $BUILD"
[[ -f "$GNB_IDEAL" && -f "$UE_CONF" ]] || die "OAI RFsim configuration is missing"

prepare_awgn_config() {
  [[ -f "$GNB_AWGN" ]] && return 0
  mkdir -p "$(dirname "$GNB_AWGN")"
  cp "$GNB_IDEAL" "$GNB_AWGN"
  cat >>"$GNB_AWGN" <<'AWGN_CONFIG'

channelmod = {
  max_chan = 10;
  modellist = "modellist_rfsimu_1";
  modellist_rfsimu_1 = (
    { model_name = "rfsimu_channel_enB0"; type = "AWGN"; ploss_dB = 0; noise_power_dB = -100; forgetfact = 0; offset = 0; ds_tdl = 0; },
    { model_name = "rfsimu_channel_ue0";  type = "AWGN"; ploss_dB = 0; noise_power_dB = -95;  forgetfact = 0; offset = 0; ds_tdl = 0; }
  );
};
AWGN_CONFIG
  echo "Created AWGN configuration: $GNB_AWGN"
}

if [[ "$MODE" == awgn || "$MODE" == pair ]]; then
  prepare_awgn_config
fi

mkdir -p "$RESULTS_DIR"

# Authenticate once so background processes never stop to request a password.
sudo -v

GNB_PID=""
UE_PID=""
GNB_UNIT=""
UE_UNIT=""

stop_unit() {
  local unit="$1"
  [[ -n "$unit" ]] || return 0
  sudo systemctl stop "$unit" 2>/dev/null || true
  sudo systemctl reset-failed "$unit" 2>/dev/null || true
}

stop_oai_process() {
  local name="$1"
  pgrep -x "$name" >/dev/null || return 0
  sudo pkill -INT -x "$name" 2>/dev/null || true
  for _ in $(seq 1 10); do
    pgrep -x "$name" >/dev/null || return 0
    sleep 1
  done
  sudo pkill -KILL -x "$name" 2>/dev/null || true
}

if pgrep -x nr-softmodem >/dev/null || pgrep -x nr-uesoftmodem >/dev/null; then
  echo "Found stale OAI processes; stopping them before the experiment"
  stop_oai_process nr-uesoftmodem
  stop_oai_process nr-softmodem
  pgrep -x nr-softmodem >/dev/null && die "could not stop the existing gNB"
  pgrep -x nr-uesoftmodem >/dev/null && die "could not stop the existing nrUE"
fi

cleanup() {
  stop_unit "$UE_UNIT"
  stop_unit "$GNB_UNIT"
  stop_oai_process nr-uesoftmodem
  stop_oai_process nr-softmodem
}
trap cleanup EXIT INT TERM

wait_for_gnb() {
  local log="$1"
  for _ in $(seq 1 60); do
    if ss -ltn 2>/dev/null | grep -qE '[:.]4043[[:space:]]'; then
      return 0
    fi
    if ! sudo systemctl is-active --quiet "$GNB_UNIT"; then
      die "gNB exited before RFsim became ready; see $log"
    fi
    sleep 0.5
  done
  die "RFsim port 4043 did not become ready; see $log"
}

wait_for_sync() {
  local log="$1"
  for _ in $(seq 1 "$SYNC_TIMEOUT"); do
    if grep -q 'UE synchronized!' "$log" 2>/dev/null; then
      return 0
    fi
    # sudo/taskset may briefly change the observable PID/process name while it
    # hands execution to nr-uesoftmodem.  Treating that transition as process
    # death races with UE startup and makes the EXIT trap kill a healthy UE.
    # Only fail early for an explicit fatal marker; otherwise let the complete
    # synchronization timeout decide the result.
    if grep -Eqi 'segmentation fault|assertion .* failed|fatal error|core dumped' "$log" 2>/dev/null; then
      return 2
    fi
    sleep 1
  done
  return 1
}

capture_stats() {
  local source_file="$1"
  local target_file="$2"
  local final_marker="$3"
  local tmp_file="${target_file}.tmp"

  for _ in $(seq 1 15); do
    if sudo test -s "$source_file" 2>/dev/null; then
      sudo cp "$source_file" "$tmp_file"
      sudo chown "$USER:$(id -gn)" "$tmp_file"
      if grep -q "$final_marker" "$tmp_file" 2>/dev/null; then
        mv "$tmp_file" "$target_file"
        return 0
      fi
      rm -f "$tmp_file"
    fi
    sleep 1
  done
  die "could not capture a complete statistics snapshot from $source_file"
}

append_window_rows() {
  local side="$1"
  local start_file="$2"
  local end_file="$3"
  local output_file="$4"

  LC_ALL=C awk -v side="$side" '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function parse(line, p, c, f) {
      c = index(line, ":")
      if (c == 0) return 0
      parsed_name = trim(substr(line, 1, c - 1))
      if (split(substr(line, c + 1), f, ";") < 3) return 0
      parsed_avg = f[1]
      parsed_count = f[2]
      parsed_max = f[3]
      gsub(/[^0-9.eE+-]/, "", parsed_avg)
      gsub(/[^0-9]/, "", parsed_count)
      gsub(/[^0-9.eE+-]/, "", parsed_max)
      return parsed_name != "" && parsed_avg != "" && parsed_count != "" && parsed_max != ""
    }
    NR == FNR {
      if (parse($0)) {
        start_avg[parsed_name] = parsed_avg + 0
        start_count[parsed_name] = parsed_count + 0
      }
      next
    }
    {
      if (!parse($0) || !(parsed_name in start_count)) next
      delta_count = (parsed_count + 0) - start_count[parsed_name]
      if (delta_count <= 0) next
      delta_total = (parsed_avg + 0) * (parsed_count + 0) \
                    - start_avg[parsed_name] * start_count[parsed_name]
      window_avg = delta_total / delta_count
      csv_name = parsed_name
      gsub(/"/, "\"\"", csv_name)
      printf "%s,\"%s\",%.6f,%d,%.6f,%d,%.6f,%d,%.6f\n", \
             side, csv_name, window_avg, delta_count, \
             start_avg[parsed_name], start_count[parsed_name], \
             parsed_avg + 0, parsed_count + 0, parsed_max + 0
    }
  ' "$start_file" "$end_file" >>"$output_file"
}

build_window_csv() {
  local prefix="$1"
  local output_file="$prefix-window-stats.csv"

  echo 'side,module,window_avg_us,window_calls,start_avg_us,start_calls,end_avg_us,end_calls,end_cumulative_max_us' >"$output_file"
  append_window_rows gNB "$prefix-start-nrL1_stats.log" "$prefix-end-nrL1_stats.log" "$output_file"
  append_window_rows nrUE "$prefix-start-nrL1_UE_stats-0.log" "$prefix-end-nrL1_UE_stats-0.log" "$output_file"
  [[ $(wc -l <"$output_file") -gt 1 ]] || die "window statistics contain no positive call-count deltas"
}

run_once() {
  local channel="$1"
  local run_no="$2"
  local prefix="$RESULTS_DIR/${channel}-run${run_no}"
  local gnb_conf="$GNB_IDEAL"
  local -a channel_args=()

  if [[ "$channel" == awgn ]]; then
    gnb_conf="$GNB_AWGN"
    channel_args=(--rfsimulator.options chanmod)
  fi

  for suffix in gnb.log nrue.log nrL1_stats.log nrL1_UE_stats-0.log \
                start-nrL1_stats.log start-nrL1_UE_stats-0.log \
                end-nrL1_stats.log end-nrL1_UE_stats-0.log \
                window-stats.csv metadata.txt; do
    [[ ! -e "$prefix-$suffix" ]] || die "Result already exists: $prefix-$suffix"
  done

  echo "[$channel run $run_no] cleaning old statistics"
  sudo rm -f "$BUILD/nrL1_stats.log" "$BUILD/nrL1_UE_stats-0.log"

  echo "[$channel run $run_no] starting gNB"
  GNB_UNIT="k3-hotspot-${channel}-run${run_no}-gnb.service"
  : >"$prefix-gnb.log"
  stop_unit "$GNB_UNIT"
  sudo systemd-run --quiet --unit="$GNB_UNIT" --service-type=exec --collect \
    --working-directory="$BUILD" \
    --property="AllowedCPUs=$CPU_SET" \
    --property=LimitMEMLOCK=infinity \
    --property=TasksMax=infinity \
    --property="StandardOutput=append:$prefix-gnb.log" \
    --property="StandardError=append:$prefix-gnb.log" \
    "$BUILD/nr-softmodem" \
      -O "$gnb_conf" \
      --rfsim \
      --rfsimulator.serveraddr server \
      "${channel_args[@]}" \
      --phy-test \
      --noS1 \
      --gNBs.[0].min_rxtxtime 6 \
      --T_stdout 1 \
      -q
  GNB_PID="$(sudo systemctl show -p MainPID --value "$GNB_UNIT")"
  wait_for_gnb "$prefix-gnb.log"

  local sync_attempt=1
  : >"$prefix-nrue.log"
  while :; do
    echo "[$channel run $run_no] starting nrUE (sync attempt $sync_attempt/$SYNC_ATTEMPTS)"
    printf '\n===== nrUE synchronization attempt %d/%d =====\n' \
      "$sync_attempt" "$SYNC_ATTEMPTS" >>"$prefix-nrue.log"
    UE_UNIT="k3-hotspot-${channel}-run${run_no}-nrue-a${sync_attempt}.service"
    stop_unit "$UE_UNIT"
    sudo systemd-run --quiet --unit="$UE_UNIT" --service-type=exec --collect \
      --working-directory="$BUILD" \
      --property="AllowedCPUs=$CPU_SET" \
      --property=LimitMEMLOCK=infinity \
      --property=TasksMax=infinity \
      --property="StandardOutput=append:$prefix-nrue.log" \
      --property="StandardError=append:$prefix-nrue.log" \
      "$BUILD/nr-uesoftmodem" \
        -O "$UE_CONF" \
        --rfsim \
        --rfsimulator.serveraddr 127.0.0.1 \
        --phy-test \
        --noS1 \
        -r 106 \
        --numerology 1 \
        --band 78 \
        -C 3319680000 \
        --ue-rxgain 140 \
        --ue-txgain 0 \
        --T_stdout 1 \
        -q
    UE_PID="$(sudo systemctl show -p MainPID --value "$UE_UNIT")"

    local sync_rc=0
    if wait_for_sync "$prefix-nrue.log"; then
      break
    else
      sync_rc=$?
    fi
    if (( sync_rc == 2 )); then
      die "nrUE reported a fatal error before synchronization; see $prefix-nrue.log"
    fi
    if (( sync_attempt >= SYNC_ATTEMPTS )); then
      die "nrUE did not synchronize after $SYNC_ATTEMPTS attempts; see $prefix-nrue.log"
    fi
    echo "[$channel run $run_no] synchronization attempt $sync_attempt timed out; restarting nrUE"
    stop_unit "$UE_UNIT"
    UE_UNIT=""
    stop_oai_process nr-uesoftmodem
    UE_PID=""
    sleep 2
    sync_attempt=$((sync_attempt + 1))
  done
  local sync_time
  sync_time="$(date --iso-8601=seconds)"
  echo "[$channel run $run_no] synchronized; warm-up ${WARMUP_SECONDS}s"
  sleep "$WARMUP_SECONDS"

  echo "[$channel run $run_no] capturing start snapshots"
  capture_stats "$BUILD/nrL1_stats.log" "$prefix-start-nrL1_stats.log" 'feptx_total:'
  capture_stats "$BUILD/nrL1_UE_stats-0.log" "$prefix-start-nrL1_UE_stats-0.log" 'OFDM_MOD_STATS:'
  local measure_start
  measure_start="$(date --iso-8601=seconds)"
  echo "[$channel run $run_no] measuring ${MEASURE_SECONDS}s"
  sleep "$MEASURE_SECONDS"

  echo "[$channel run $run_no] capturing end snapshots"
  capture_stats "$BUILD/nrL1_stats.log" "$prefix-end-nrL1_stats.log" 'feptx_total:'
  capture_stats "$BUILD/nrL1_UE_stats-0.log" "$prefix-end-nrL1_UE_stats-0.log" 'OFDM_MOD_STATS:'
  local measure_end
  measure_end="$(date --iso-8601=seconds)"

  build_window_csv "$prefix"

  stop_unit "$UE_UNIT"
  UE_UNIT=""
  stop_oai_process nr-uesoftmodem
  UE_PID=""
  stop_unit "$GNB_UNIT"
  GNB_UNIT=""
  stop_oai_process nr-softmodem
  GNB_PID=""

  [[ -f "$BUILD/nrL1_stats.log" ]] || die "gNB statistics were not generated"
  [[ -f "$BUILD/nrL1_UE_stats-0.log" ]] || die "nrUE statistics were not generated"
  sudo cp "$BUILD/nrL1_stats.log" "$prefix-nrL1_stats.log"
  sudo cp "$BUILD/nrL1_UE_stats-0.log" "$prefix-nrL1_UE_stats-0.log"
  sudo chown "$USER:$(id -gn)" "$prefix-nrL1_stats.log" "$prefix-nrL1_UE_stats-0.log"

  {
    echo "channel=$channel"
    echo "run=$run_no"
    echo "sync_time=$sync_time"
    echo "measure_start=$measure_start"
    echo "measure_end=$measure_end"
    echo "warmup_seconds=$WARMUP_SECONDS"
    echo "measure_seconds=$MEASURE_SECONDS"
    echo "sync_timeout_seconds=$SYNC_TIMEOUT"
    echo "sync_attempts_used=$sync_attempt"
    echo "statistics_scope=two_snapshot_window_difference"
    echo "maximum_scope=process_cumulative_at_end_snapshot"
    echo "cpu_set=$CPU_SET"
    echo "launcher=systemd-transient-service"
    echo "kernel=$(uname -r)"
    echo "gnb_binary_sha256=$(sha256sum "$BUILD/nr-softmodem" | awk '{print $1}')"
    echo "nrue_binary_sha256=$(sha256sum "$BUILD/nr-uesoftmodem" | awk '{print $1}')"
    echo "oai_commit=$(git -C "$OAI" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "oai_worktree_dirty=$(test -n "$(git -C "$OAI" status --porcelain 2>/dev/null)" && echo yes || echo no)"
    echo "gnb_config_sha256=$(sha256sum "$gnb_conf" | awk '{print $1}')"
    echo "ue_config_sha256=$(sha256sum "$UE_CONF" | awk '{print $1}')"
  } >"$prefix-metadata.txt"

  echo "[$channel run $run_no] completed: $prefix-*"
}

LAST_RUN=$((START_RUN + REPEATS - 1))
for run_no in $(seq "$START_RUN" "$LAST_RUN"); do
  if [[ "$MODE" == ideal || "$MODE" == pair ]]; then
    run_once ideal "$run_no"
  fi
  if [[ "$MODE" == pair ]]; then
    echo "Cooling down for ${COOLDOWN_SECONDS}s"
    sleep "$COOLDOWN_SECONDS"
  fi
  if [[ "$MODE" == awgn || "$MODE" == pair ]]; then
    run_once awgn "$run_no"
  fi
  if [[ "$run_no" -lt "$LAST_RUN" ]]; then
    echo "Cooling down for ${COOLDOWN_SECONDS}s"
    sleep "$COOLDOWN_SECONDS"
  fi
done

trap - EXIT INT TERM
echo "All requested runs completed. Results: $RESULTS_DIR"
