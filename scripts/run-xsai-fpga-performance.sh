#!/bin/busybox sh
# Run an OAI gNB + nrUE RFsim performance window in the XSAI FPGA initramfs.

set -u

OAI_ROOT="${OAI_BUNDLE_ROOT:-/opt/oai}"
BUILD="$OAI_ROOT"
GNB="$BUILD/bin/nr-softmodem"
NRUE="$BUILD/bin/nr-uesoftmodem"
GNB_CONF="$BUILD/etc/gnb.sa.band78.24prb.rfsim.conf"
UE_CONF="$BUILD/etc/ue.conf"
RESULTS_DIR="${RESULTS_DIR:-/results/oai-xsai-newwork}"
SYNC_TIMEOUT="${SYNC_TIMEOUT:-1800}"
GNB_READY_TIMEOUT="${GNB_READY_TIMEOUT:-900}"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
MEASUREMENT_SECONDS="${MEASUREMENT_SECONDS:-120}"
FRAME_MARKER_INTERVAL="${FRAME_MARKER_INTERVAL:-128}"
HEARTBEAT_INTERVAL="${HEARTBEAT_INTERVAL:-60}"
CLEANUP_TIMEOUT="${CLEANUP_TIMEOUT:-15}"
STATS_READY_TIMEOUT="${STATS_READY_TIMEOUT:-30}"
OAI_KEEP_EVENTS="${OAI_KEEP_EVENTS:-0}"
GNB_THREAD_POOL="${GNB_THREAD_POOL:--1}"
UE_THREAD_POOL="${UE_THREAD_POOL:--1,-1}"
REPEATS="${1:-1}"
START_RUN="${2:-1}"

export PATH=/opt/oai/bin:/opt/oai/scripts:/bin:/sbin:/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/opt/oai/lib

die() {
  echo "[oai-xsai-perf] ERROR: $*" >&2
  exit 1
}

is_positive_integer() {
  case "$1" in
    ''|*[!0-9]*|0) return 1 ;;
    *) return 0 ;;
  esac
}
is_nonnegative_integer() {
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
    *) return 0 ;;
  esac
}

is_positive_integer "$SYNC_TIMEOUT" || die "invalid SYNC_TIMEOUT=$SYNC_TIMEOUT"
is_positive_integer "$GNB_READY_TIMEOUT" || die "invalid GNB_READY_TIMEOUT=$GNB_READY_TIMEOUT"
is_nonnegative_integer "$WARMUP_SECONDS" || die "invalid WARMUP_SECONDS=$WARMUP_SECONDS"
is_positive_integer "$MEASUREMENT_SECONDS" || die "invalid MEASUREMENT_SECONDS=$MEASUREMENT_SECONDS"
is_positive_integer "$FRAME_MARKER_INTERVAL" || die "invalid FRAME_MARKER_INTERVAL=$FRAME_MARKER_INTERVAL"
is_positive_integer "$HEARTBEAT_INTERVAL" || die "invalid HEARTBEAT_INTERVAL=$HEARTBEAT_INTERVAL"
is_positive_integer "$CLEANUP_TIMEOUT" || die "invalid CLEANUP_TIMEOUT=$CLEANUP_TIMEOUT"
is_positive_integer "$STATS_READY_TIMEOUT" || die "invalid STATS_READY_TIMEOUT=$STATS_READY_TIMEOUT"
is_positive_integer "$REPEATS" || die "repeat count must be positive"
is_positive_integer "$START_RUN" || die "start run must be positive"
[ "$REPEATS" -eq 1 ] || die 'NEWwork exports exactly one CSV per boot'
[ -x "$GNB" ] || die "missing $GNB"
[ -x "$NRUE" ] || die "missing $NRUE"
[ -r "$GNB_CONF" ] || die "missing $GNB_CONF"
[ -r "$UE_CONF" ] || die "missing $UE_CONF"

mkdir -p "$RESULTS_DIR"
RESULTS_DIR=$(realpath "$RESULTS_DIR") || die "cannot resolve results directory"
ip link set lo up 2>/dev/null || true

gnb_pid=""
nrue_pid=""
gnb_monitor_pid=""
nrue_monitor_pid=""
gnb_fifo=""
nrue_fifo=""
gnb_pgid=""
nrue_pgid=""
cleanup_done=0
cleanup_status=0
work_dir=""
gnb_events=""
nrue_events=""

read_process_stat() {
  local line
  read -r line 2>/dev/null <"/proc/$1/stat" || return 1
  # comm may contain spaces or parentheses; fields after the last ')' are fixed.
  set -- ${line##*) }
  [ "$#" -ge 4 ] || return 1
  proc_state=$1 proc_pgid=$3 proc_sid=$4
}

process_is_live() {
  [ -n "$1" ] && read_process_stat "$1" || return 1
  case "$proc_state" in Z|X|x) return 1 ;; esac
  return 0
}

confirm_workload_group() {
  local pid="$1" attempts=0
  # Noninteractive setsid must retain the tracked PID as its PGID/session ID.
  while [ "$attempts" -lt 5 ]; do
    if process_is_live "$pid" && [ "$proc_pgid" = "$pid" ] && [ "$proc_sid" = "$pid" ]; then
      return 0
    fi
    attempts=$((attempts + 1))
    sleep 1
  done
  return 1
}

signal_workload() {
  local signal="$1" pid="$2" pgid="$3"
  if [ -n "$pgid" ] && [ "$pgid" -gt 1 ] && [ "$pgid" != "$$" ]; then
    # PGIDs are recorded only after confirm_workload_group succeeds.
    kill "-$signal" -- "-$pgid" 2>/dev/null || true
  elif [ -n "$pid" ]; then
    kill "-$signal" "$pid" 2>/dev/null || true
  fi
}

stop_process() {
  # Measurement and result snapshots are already complete. Do not CONT/INT:
  # resuming OAI can re-enter its shutdown handlers or flood a stopped reader.
  signal_workload KILL "$1" "${2:-}"
}

stop_monitor() {
  local pid="$1"
  [ -n "$pid" ] || return 0
  kill -KILL "$pid" 2>/dev/null || true
  # No wait builtin here: even SIGKILL does not bound a kernel-side exit.
}

cleanup_pending() {
  local p stat_file member
  pending=""
  for p in "$gnb_pid" "$nrue_pid" "$gnb_monitor_pid" "$nrue_monitor_pid"; do
    if process_is_live "$p"; then pending="$pending $p:$proc_state"; fi
  done
  # Include OAI's forked helpers, not just the two group leaders. Ignore zombies:
  # they are no longer executing and will be reaped by the shell or init.
  if [ -n "$gnb_pgid$nrue_pgid" ]; then
    for stat_file in /proc/[0-9]*/stat; do
      member=${stat_file#/proc/}; member=${member%/stat}
      process_is_live "$member" || continue
      if { [ -n "$gnb_pgid" ] && [ "$proc_pgid" = "$gnb_pgid" ]; } ||
         { [ -n "$nrue_pgid" ] && [ "$proc_pgid" = "$nrue_pgid" ]; }; then
        case " $pending " in *" $member:"*) ;; *) pending="$pending $member:$proc_state" ;; esac
      fi
    done
  fi
  [ -n "$pending" ]
}

read_counter() {
  local file="$1" value=""
  read -r value <"$file" || return 1
  case "$value" in
    ''|*[!0-9]*) return 1 ;;
  esac
  echo "$value"
}

# /proc/uptime is monotonic guest time, independent of timer-loop scheduling.
uptime_seconds() {
  local value rest
  read -r value rest </proc/uptime || return 1
  echo "${value%%.*}"
}

cpu_ticks() {
  local line
  read -r line 2>/dev/null <"/proc/$1/stat" || return 1
  set -- ${line##*) }
  [ "$#" -ge 13 ] || return 1
  echo "$((${12} + ${13}))"
}

# OAI creates these statistics threads at RR/1. On a busy single-HART FPGA,
# RR/97 radio workers may prevent them from ever writing a fresh sample.
# They sleep between samples, so briefly running them at RR/98 lets the
# cumulative per-module counters advance without changing radio worker policy.
promote_stats_threads() {
  local pid="$1" expected="$2" task comm tid name found=0
  for task in /proc/"$pid"/task/[0-9]*; do
    [ -r "$task/comm" ] || continue
    read -r name <"$task/comm" || continue
    case "$name" in
      "$expected")
        tid=${task##*/}
        chrt -r -p 98 "$tid" >/dev/null 2>&1 || die "cannot schedule $name (tid=$tid) for performance sampling"
        found=$((found + 1))
        ;;
    esac
  done
  echo "[oai-xsai-perf] stats_thread=$expected priority=RR:98 count=$found"
}

wait_for_stats_files() {
  local started now elapsed
  started=$(uptime_seconds) || return 1
  while :; do
    if [ -s "$work_dir/nrL1_stats.log" ] && [ -s "$work_dir/nrL1_UE_stats-0.log" ]; then
      return 0
    fi
    now=$(uptime_seconds) || return 1
    elapsed=$((now - started))
    [ "$elapsed" -lt "$STATS_READY_TIMEOUT" ] || return 1
    process_is_live "$gnb_pid" && process_is_live "$nrue_pid" || return 1
    sleep 1
  done
}

# The rvv_modify-data-stream experiment compares cumulative OAI statistics
# immediately before and after measurement. Snapshots stay in the temporary
# work directory and are removed after the single CSV has been exported.
snapshot_stats() {
  local source="$1" target="$2"
  [ -s "$source" ] || die "missing module statistics: $source"
  cp "$source" "$target" || die "cannot snapshot module statistics: $source"
}

append_window_stats_rows() {
  local side="$1" start_file="$2" end_file="$3" output_file="$4"
  [ -s "$start_file" ] && [ -s "$end_file" ] || return 0
  LC_ALL=C awk -v side="$side" '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    function parse(line, fields, colon) {
      colon = index(line, ":")
      if (!colon) return 0
      parsed_name = trim(substr(line, 1, colon - 1))
      if (split(substr(line, colon + 1), fields, ";") < 3) return 0
      parsed_avg = fields[1]
      parsed_count = fields[2]
      parsed_max = fields[3]
      gsub(/[^0-9.eE+-]/, "", parsed_avg)
      gsub(/[^0-9]/, "", parsed_count)
      gsub(/[^0-9.eE+-]/, "", parsed_max)
      return parsed_name != "" && parsed_avg != "" && parsed_count != "" && parsed_max != ""
    }
    FILENAME == ARGV[1] {
      if (parse($0)) {
        start_avg[parsed_name] = parsed_avg + 0
        start_count[parsed_name] = parsed_count + 0
      }
      next
    }
    {
      if (parse($0)) {
        end_avg[parsed_name] = parsed_avg + 0
        end_count[parsed_name] = parsed_count + 0
        end_max[parsed_name] = parsed_max + 0
      }
    }
    END {
      for (name in end_count) {
        if (!(name in start_count)) continue
        delta_count = end_count[name] - start_count[name]
        if (delta_count <= 0) continue
        delta_total = end_avg[name] * end_count[name] - start_avg[name] * start_count[name]
        window_avg = delta_total / delta_count
        key = name
        gsub(/[^A-Za-z0-9]+/, "_", key)
        sub(/^_+/, "", key)
        sub(/_+$/, "", key)
        if (key == "") continue
        printf "%s_%s_window_avg_us,%.6f,microseconds\n", side, key, window_avg
        printf "%s_%s_window_calls,%d,calls\n", side, key, delta_count
        printf "%s_%s_start_calls,%d,calls\n", side, key, start_count[name]
        printf "%s_%s_end_calls,%d,calls\n", side, key, end_count[name]
        printf "%s_%s_end_cumulative_max_us,%.6f,microseconds\n", side, key, end_max[name]
      }
    }
  ' "$start_file" "$end_file" >>"$output_file"
}

prepare_scheduler() {
  # OAI's threadCreate uses SCHED_RR up to 97 when root has CAP_SYS_NICE.
  # Keep short, blocking control/collector tasks above those workers. OAI
  # MUST explicitly start with OTHER so its main thread cannot inherit 99.
  chrt -r -p 99 "$$" >/dev/null 2>&1 || die "cannot protect monitor: SCHED_RR/99 unavailable"
  echo '[oai-xsai-perf] monitor_version=rt-fifo-v3 supervisor=RR:99 collectors=RR:98 workload_initial=OTHER:0'
}

freeze_workload() {
  signal_workload STOP "$nrue_pid" "$nrue_pgid"
  signal_workload STOP "$gnb_pid" "$gnb_pgid"
}

diagnose() {
  local label="$1" p
  echo "[oai-xsai-perf] diagnostic stage=$label"
  for p in "$gnb_pid" "$nrue_pid" "$gnb_monitor_pid" "$nrue_monitor_pid"; do
    [ -n "$p" ] || continue
    echo "[oai-xsai-perf] pid=$p"
    cat "/proc/$p/stat" 2>/dev/null || true
    cat "/proc/$p/wchan" 2>/dev/null || true
    echo
  done
}

start_gnb_monitor() {
  local fifo="$1" event_file="$2" counter_file="$3"
  mkfifo "$fifo" || die "cannot create $fifo"
  : >"$event_file"
  echo 0 >"$counter_file"
  OAI_KEEP_EVENTS="${OAI_KEEP_EVENTS:-0}" OAI_MONITOR_STATE="$counter_file" OAI_MONITOR_NEXT="$counter_file.next" \
  chrt -r 98 /bin/busybox awk -v events="$event_file" -v counter="$counter_file" '
    function keep(line) {
      if (ENVIRON["OAI_KEEP_EVENTS"] != "1") return
      # Bound disk use, even for repeated errors: two 32-line chunks/side.
      if (kept++ % 32 == 0) {
        if (kept > 1) {
          while ((getline old < events) > 0) print old > (events ".prev")
          close(events); close(events ".prev")
        }
        printf "" > events
        close(events)
      }
      print substr(line, 1, 512) >> events
      close(events)
    }
    /Frame\.Slot/ {
      markers++
      print markers > ENVIRON["OAI_MONITOR_NEXT"]
      close(ENVIRON["OAI_MONITOR_NEXT"])
      if (system("mv -f \"$OAI_MONITOR_NEXT\" \"$OAI_MONITOR_STATE\"") != 0) exit 2
      keep($0)
      next
    }
    /LDPC decoder:|L1 Tx processing:|L1 Rx processing:|MAC:.*TX/ {
      keep($0)
      next
    }
    {
      lower = tolower($0)
      if (lower ~ /error|fatal|assert|segmentation|core dumped|failed/)
        keep($0)
    }
  ' <"$fifo" &
  gnb_monitor_pid=$!
}

start_nrue_monitor() {
  local fifo="$1" event_file="$2" state_file="$3"
  mkfifo "$fifo" || die "cannot create $fifo"
  : >"$event_file"
  echo WAIT >"$state_file"
  OAI_KEEP_EVENTS="${OAI_KEEP_EVENTS:-0}" OAI_MONITOR_STATE="$state_file" OAI_MONITOR_NEXT="$state_file.next" \
  chrt -r 98 /bin/busybox awk -v events="$event_file" -v state="$state_file" '
    function publish(value) {
      print value > ENVIRON["OAI_MONITOR_NEXT"]
      close(ENVIRON["OAI_MONITOR_NEXT"])
      if (system("mv -f \"$OAI_MONITOR_NEXT\" \"$OAI_MONITOR_STATE\"") != 0) exit 2
    }
    function keep(line) {
      if (ENVIRON["OAI_KEEP_EVENTS"] != "1") return
      if (kept++ % 32 == 0) {
        if (kept > 1) {
          while ((getline old < events) > 0) print old > (events ".prev")
          close(events); close(events ".prev")
        }
        printf "" > events
        close(events)
      }
      print substr(line, 1, 512) >> events
      close(events)
    }
    /UE synchronized!/ {
      if (!synced) publish("SYNC")
      synced=1
      keep($0)
      next
    }
    /LDPC decoder:|L1 Tx processing:|L1 Rx processing:/ {
      keep($0)
      next
    }
    {
      lower = tolower($0)
      # Ordinary RFsim decode errors are not process-fatal.
      if (lower ~ /assertion.*failed|assertfatal|fatal error|segmentation fault|core dumped/) {
        publish("FATAL")
        keep($0)
        next
      }
      if (lower ~ /error|fatal|assert|failed/) {
        keep($0)
      }
    }
  ' <"$fifo" &
  nrue_monitor_pid=$!
}

wait_for_frame_markers() {
  local counter_file="$1" baseline="$2" requested="$3" pid="$4" seconds="$5"
  local target=$((baseline + requested)) current completed state
  local started now elapsed next_heartbeat=0
  started=$(uptime_seconds) || return 4
  while :; do
    now=$(uptime_seconds) || return 4
    elapsed=$((now - started))
    [ "$elapsed" -lt "$seconds" ] || return 1
    kill -0 "$pid" 2>/dev/null || return 2
    kill -0 "$nrue_pid" 2>/dev/null || return 2
    kill -0 "$nrue_monitor_pid" 2>/dev/null || return 3
    if [ -n "$gnb_monitor_pid" ] && ! kill -0 "$gnb_monitor_pid" 2>/dev/null; then
      return 3
    fi
    read -r state <"$nrue_state" || return 4
    [ "$state" != FATAL ] || return 2
    current=$(read_counter "$counter_file") || return 4
    if [ "$current" -ge "$target" ]; then
      observed_frame_markers=$((current - baseline))
      echo "[oai-xsai-perf] finite-frame target reached ${observed_frame_markers}/${requested}; freezing workload"
      freeze_workload
      return 0
    fi
    if [ "$elapsed" -ge "$next_heartbeat" ]; then
      completed=$((current - baseline))
      [ "$completed" -lt 0 ] && completed=0
      echo "[oai-xsai-perf] finite-frame progress ${completed}/${requested} markers (${elapsed}/${seconds}s)"
      next_heartbeat=$((elapsed + HEARTBEAT_INTERVAL))
    fi
    sleep 5
  done
  return 1
}

# Time-bounded warm-up and measurement. Zero observed markers is valid and
# must not be misreported as zero throughput.
wait_for_measurement_window() {
  local counter_file="$1" baseline="$2" pid="$3" seconds="$4" phase="${5:-measurement}"
  local started now elapsed current state
  started=$(uptime_seconds) || return 4
  while :; do
    now=$(uptime_seconds) || return 4
    elapsed=$((now - started))
    kill -0 "$pid" 2>/dev/null || return 2
    kill -0 "$nrue_pid" 2>/dev/null || return 2
    kill -0 "$gnb_monitor_pid" 2>/dev/null || return 3
    kill -0 "$nrue_monitor_pid" 2>/dev/null || return 3
    read -r state <"$nrue_state" || return 4
    [ "$state" != FATAL ] || return 2
    current=$(read_counter "$counter_file") || return 4
    if [ "$elapsed" -ge "$seconds" ]; then
      observed_frame_markers=$((current - baseline))
      [ "$observed_frame_markers" -ge 0 ] || observed_frame_markers=0
      [ "$phase" = warmup ] || freeze_workload
      return 0
    fi
    sleep 1
  done
}

cleanup() {
  local started now elapsed next_heartbeat=5
  [ "$cleanup_done" -eq 0 ] || return "$cleanup_status"
  cleanup_done=1
  cleanup_status=0
  echo "[oai-xsai-perf] stage=cleanup policy=kill-frozen-groups timeout=${CLEANUP_TIMEOUT}s"
  started=$(uptime_seconds) || started=""
  freeze_workload
  stop_process "$nrue_pid" "$nrue_pgid"
  stop_process "$gnb_pid" "$gnb_pgid"
  stop_monitor "$nrue_monitor_pid"
  stop_monitor "$gnb_monitor_pid"
  while cleanup_pending; do
    now=$(uptime_seconds) || now=""
    if [ -z "$started" ] || [ -z "$now" ]; then
      cleanup_status=1
      break
    fi
    elapsed=$((now - started))
    if [ "$elapsed" -ge "$CLEANUP_TIMEOUT" ]; then
      cleanup_status=1
      break
    fi
    if [ "$elapsed" -ge "$next_heartbeat" ]; then
      echo "[oai-xsai-perf] cleanup pending=$pending (${elapsed}/${CLEANUP_TIMEOUT}s)"
      next_heartbeat=$((elapsed + 5))
    fi
    sleep 1
  done
  if [ "$cleanup_status" -ne 0 ]; then
    echo "[oai-xsai-perf] OAI_XSAI_CLEANUP_RESULT=FAIL pending=$pending"
    diagnose cleanup-deadline
    return 1
  fi
  gnb_pid="" nrue_pid="" gnb_monitor_pid="" nrue_monitor_pid=""
  gnb_pgid="" nrue_pgid=""
  [ -n "$nrue_fifo" ] && rm -f "$nrue_fifo"
  [ -n "$gnb_fifo" ] && rm -f "$gnb_fifo"
  nrue_fifo="" gnb_fifo=""
  echo '[oai-xsai-perf] OAI_XSAI_CLEANUP_RESULT=PASS'
  return 0
}
purge_transient() {
  # work_dir is created by this script under the resolved results directory.
  # Never recurse outside that exact per-run location.
  case "$work_dir" in
    "$RESULTS_DIR"/work-run[0-9]*)
      [ -d "$work_dir" ] && rm -rf -- "$work_dir"
      ;;
  esac
  [ -z "$gnb_events" ] || rm -f -- "$gnb_events" "$gnb_events.prev"
  [ -z "$nrue_events" ] || rm -f -- "$nrue_events" "$nrue_events.prev"
}
on_exit() {
  local status=$?
  trap - EXIT INT TERM
  if ! cleanup; then [ "$status" -ne 0 ] || status=1; fi
  purge_transient
  exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port_is_listening() {
  local proc_tcp slot local_address remote_address state remainder
  # Keep this check inside the shell.  Starting netstat and grep once per
  # second is expensive enough to starve the monitor on the single-HART FPGA.
  for proc_tcp in /proc/net/tcp /proc/net/tcp6; do
    [ -r "$proc_tcp" ] || continue
    while read -r slot local_address remote_address state remainder; do
      case "$local_address" in
        *:0FCB)
          [ "$state" = "0A" ] && return 0
          ;;
      esac
    done <"$proc_tcp"
  done
  return 1
}

wait_for_port() {
  local pid="$1" seconds="$2" started now elapsed next_heartbeat=0
  started=$(uptime_seconds) || return 4
  while :; do
    now=$(uptime_seconds) || return 4
    elapsed=$((now - started))
    [ "$elapsed" -lt "$seconds" ] || return 1
    kill -0 "$pid" 2>/dev/null || return 2
    kill -0 "$gnb_monitor_pid" 2>/dev/null || return 3
    if port_is_listening; then
      return 0
    fi
    if [ "$elapsed" -ge "$next_heartbeat" ]; then
      echo "[oai-xsai-perf] waiting for gNB RFsim port 4043 (${elapsed}/${seconds})"
      next_heartbeat=$((elapsed + HEARTBEAT_INTERVAL))
    fi
    sleep 1
  done
  return 1
}

wait_for_sync_state() {
  local state_file="$1" pid="$2" seconds="$3" state
  local started now elapsed next_heartbeat=0
  started=$(uptime_seconds) || return 4
  while :; do
    now=$(uptime_seconds) || return 4
    elapsed=$((now - started))
    [ "$elapsed" -lt "$seconds" ] || return 1
    kill -0 "$gnb_pid" 2>/dev/null || return 2
    kill -0 "$gnb_monitor_pid" 2>/dev/null || return 3
    if [ -n "$nrue_monitor_pid" ] && ! kill -0 "$nrue_monitor_pid" 2>/dev/null; then
      return 3
    fi
    read -r state <"$state_file" || return 4
    [ "$state" = SYNC ] && return 0
    [ "$state" = FATAL ] && return 2
    kill -0 "$pid" 2>/dev/null || return 2
    if [ "$elapsed" -ge "$next_heartbeat" ]; then
      echo "[oai-xsai-perf] waiting for nrUE synchronization (${elapsed}/${seconds})"
      next_heartbeat=$((elapsed + HEARTBEAT_INTERVAL))
    fi
    sleep 1
  done
  return 1
}

run_once() {
  cleanup_done=0
  cleanup_status=0
  run_no="$1"
  prefix="$RESULTS_DIR/ideal-run$run_no"
  work_dir="$RESULTS_DIR/work-run$run_no"
  [ ! -e "$work_dir" ] || die "results already exist: $work_dir"
  mkdir -p "$work_dir" || die "cannot create $work_dir"
  # libconfig resolves @include files relative to the process working
  # directory in this minimal environment.
  cp "$BUILD/etc/channelmod_rfsimu_LEO_satellite.conf" "$work_dir/"
  gnb_events="$prefix-gnb-events.log"
  nrue_events="$prefix-nrue-events.log"
  gnb_counter="$work_dir/gnb-frame-markers.count"
  nrue_state="$work_dir/nrue.state"
  gnb_fifo="$work_dir/gnb-output.fifo"
  nrue_fifo="$work_dir/nrue-output.fifo"

  echo "[oai-xsai-perf] run=$run_no profile=newwork channel=ideal prbs=24 warmup=${WARMUP_SECONDS}s measurement=${MEASUREMENT_SECONDS}s"
  prepare_scheduler

  cd "$work_dir" || die "cannot enter $work_dir"
  start_gnb_monitor "$gnb_fifo" "$gnb_events" "$gnb_counter"
  setsid chrt -o 0 "$GNB" \
    -O "$GNB_CONF" \
    --rfsim \
    --rfsimulator.serveraddr server \
    --phy-test \
    --noS1 \
    --thread-pool "$GNB_THREAD_POOL" \
    --gNBs.[0].min_rxtxtime 6 \
    --T_stdout 1 \
    -q >"$gnb_fifo" 2>&1 &
  gnb_pid=$!
  confirm_workload_group "$gnb_pid" || die "cannot establish isolated gNB process group"
  gnb_pgid=$gnb_pid

  if wait_for_port "$gnb_pid" "$GNB_READY_TIMEOUT"; then :; else
    rc=$?
    freeze_workload
    diagnose "gnb-ready rc=$rc"
    tail -80 "$gnb_events" 2>/dev/null || true
    die "gNB did not open RFsim port 4043"
  fi
  echo "[oai-xsai-perf] gNB RFsim server ready"

  start_nrue_monitor "$nrue_fifo" "$nrue_events" "$nrue_state"
  setsid chrt -o 0 "$NRUE" \
    -O "$UE_CONF" \
    --rfsim \
    --rfsimulator.serveraddr 127.0.0.1 \
    --phy-test \
    --noS1 \
    --thread-pool "$UE_THREAD_POOL" \
    -r 24 \
    --ssb 24 \
    --numerology 1 \
    --band 78 \
    -C 3604800000 \
    --ue-rxgain 140 \
    --ue-txgain 0 \
    --T_stdout 1 \
    -q >"$nrue_fifo" 2>&1 &
  nrue_pid=$!
  confirm_workload_group "$nrue_pid" || die "cannot establish isolated nrUE process group"
  nrue_pgid=$nrue_pid

  if wait_for_sync_state "$nrue_state" "$nrue_pid" "$SYNC_TIMEOUT"; then :; else
    rc=$?
    freeze_workload
    diagnose "ue-sync rc=$rc"
    tail -120 "$nrue_events" 2>/dev/null || true
    die "nrUE did not synchronize"
  fi
  echo "[oai-xsai-perf] UE synchronized; warming up for ${WARMUP_SECONDS}s"
  promote_stats_threads "$gnb_pid" L1_stats
  promote_stats_threads "$nrue_pid" L1_UE_stats_0
  wait_for_stats_files || die "gNB/nrUE module statistics did not appear within ${STATS_READY_TIMEOUT}s"
  warmup_start_uptime=$(uptime_seconds)
  warmup_markers=$(read_counter "$gnb_counter") || die 'invalid warm-up counter'
  if wait_for_measurement_window "$gnb_counter" "$warmup_markers" "$gnb_pid" "$WARMUP_SECONDS" warmup; then :; else
    rc=$?
    die "warm-up failed rc=$rc"
  fi
  warmup_end_uptime=$(uptime_seconds)
  warmup_elapsed_seconds=$((warmup_end_uptime - warmup_start_uptime))
  snapshot_stats "$work_dir/nrL1_stats.log" "$work_dir/start-nrL1_stats.log"
  snapshot_stats "$work_dir/nrL1_UE_stats-0.log" "$work_dir/start-nrL1_UE_stats-0.log"
  start_markers=$(read_counter "$gnb_counter") || die "invalid initial counter"
  start_uptime=$(uptime_seconds)
  start_gnb_ticks=$(cpu_ticks "$gnb_pid") || die 'cannot sample gNB CPU ticks'
  start_nrue_ticks=$(cpu_ticks "$nrue_pid") || die 'cannot sample nrUE CPU ticks'
  echo "[oai-xsai-perf] measurement window started seconds=$MEASUREMENT_SECONDS"
  if wait_for_measurement_window "$gnb_counter" "$start_markers" "$gnb_pid" "$MEASUREMENT_SECONDS"; then :; else
    rc=$?
    freeze_workload
    diagnose "measurement rc=$rc (1=timeout 2=workload-exit/fatal 3=collector-exit 4=invalid-state)"
    tail -100 "$gnb_events" 2>/dev/null || true
    die "${MEASUREMENT_SECONDS}-second measurement window failed"
  fi
  end_uptime=$(uptime_seconds)
  end_gnb_ticks=$(cpu_ticks "$gnb_pid") || die 'cannot sample final gNB CPU ticks'
  end_nrue_ticks=$(cpu_ticks "$nrue_pid") || die 'cannot sample final nrUE CPU ticks'
  observed_markers=$observed_frame_markers
  confirmed_frames=$((observed_markers * FRAME_MARKER_INTERVAL))
  elapsed_seconds=$((end_uptime - start_uptime))
  [ "$elapsed_seconds" -gt 0 ] || elapsed_seconds=1
  gnb_cpu_ticks=$((end_gnb_ticks - start_gnb_ticks))
  nrue_cpu_ticks=$((end_nrue_ticks - start_nrue_ticks))

  snapshot_stats "$work_dir/nrL1_stats.log" "$work_dir/end-nrL1_stats.log"
  snapshot_stats "$work_dir/nrL1_UE_stats-0.log" "$work_dir/end-nrL1_UE_stats-0.log"
  stats_rows="$work_dir/stats-rows.csv"
  : >"$stats_rows" || die 'cannot create module statistics rows'
  append_window_stats_rows gNB "$work_dir/start-nrL1_stats.log" "$work_dir/end-nrL1_stats.log" "$stats_rows"
  gnb_stats_row_count=$(awk 'END { print NR+0 }' "$stats_rows")
  append_window_stats_rows nrUE "$work_dir/start-nrL1_UE_stats-0.log" "$work_dir/end-nrL1_UE_stats-0.log" "$stats_rows"
  stats_row_count=$(awk 'END { print NR+0 }' "$stats_rows")
  nrue_stats_row_count=$((stats_row_count - gnb_stats_row_count))
  if [ "$gnb_stats_row_count" -eq 0 ] || [ "$nrue_stats_row_count" -eq 0 ]; then
    die "module performance counters did not advance: gNB_rows=$gnb_stats_row_count nrUE_rows=$nrue_stats_row_count"
  fi

  csv_tmp="$work_dir/oai-xsai-newwork.csv"
  csv="$RESULTS_DIR/oai-xsai-newwork.csv"
  {
    echo 'metric,value,unit'
    echo 'profile,newwork,text'
    echo 'channel,ideal,text'
    echo 'prbs,24,resource_blocks'
    echo 'subcarrier_spacing_khz,30,kilohertz'
    echo "warmup_target_seconds,$WARMUP_SECONDS,seconds"
    echo "warmup_elapsed_guest_seconds,$warmup_elapsed_seconds,seconds"
    echo "measurement_target_seconds,$MEASUREMENT_SECONDS,seconds"
    echo "elapsed_guest_seconds,$elapsed_seconds,seconds"
    echo "observed_frame_markers,$observed_markers,markers"
    echo "marker_interval_frames,$FRAME_MARKER_INTERVAL,frames_per_marker"
    echo "confirmed_frames_lower_bound,$confirmed_frames,frames"
    echo "gnb_cpu_ticks_delta,$gnb_cpu_ticks,ticks"
    echo "nrue_cpu_ticks_delta,$nrue_cpu_ticks,ticks"
    if [ "$confirmed_frames" -gt 0 ]; then
      awk -v frames="$confirmed_frames" -v elapsed="$elapsed_seconds" 'BEGIN { printf "confirmed_frames_per_guest_second_lower_bound,%.6f,frames_per_second\n", frames / elapsed }'
    else
      echo 'confirmed_frames_per_guest_second_lower_bound,NA,frames_per_second'
    fi
    echo "module_stats_window_metrics,$((stats_row_count / 5)),modules"
    echo 'module_stats_window_available,1,boolean'
    cat "$stats_rows"
    echo 'result,PASS,status'
  } >"$csv_tmp" || die "cannot write $csv_tmp"
  echo '[oai-xsai-perf] stage=cleanup'
  cleanup || die 'measurement completed but cleanup failed'
  mv "$csv_tmp" "$csv" || die "cannot publish $csv"
  purge_transient
  echo '[oai-xsai-perf] OAI_XSAI_CSV_BEGIN'
  cat "$csv" || die 'cannot export CSV'
  echo '[oai-xsai-perf] OAI_XSAI_CSV_END'
  echo "[oai-xsai-perf] OAI_XSAI_MEASUREMENT_RESULT=PASS run=$run_no"
}

last_run=$((START_RUN + REPEATS - 1))
run_no="$START_RUN"
while [ "$run_no" -le "$last_run" ]; do
  run_once "$run_no"
  run_no=$((run_no + 1))
done

trap - EXIT INT TERM
echo "[oai-xsai-perf] OAI_XSAI_PERF_RESULT=PASS"
echo "[oai-xsai-perf] results=$RESULTS_DIR"
