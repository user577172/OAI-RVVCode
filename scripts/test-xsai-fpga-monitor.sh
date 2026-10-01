#!/bin/busybox sh
# Exercise the production functions, including real RR scheduling, on Linux.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_file="$script_dir/run-xsai-fpga-performance.sh"
test_dir=$(mktemp -d /tmp/oai-monitor-test.XXXXXX)
gnb_pid= nrue_pid= gnb_monitor_pid= nrue_monitor_pid= gnb_fifo= nrue_fifo=
gnb_pgid= nrue_pgid= cleanup_done=0 cleanup_status=0 CLEANUP_TIMEOUT=5
die() { echo "TEST FAIL: $*" >&2; exit 1; }
# Read only the function definitions, never the workload/main entrypoint.
eval "$(sed -n '/^read_process_stat() {/,/^run_once() {/p' "$source_file" | sed '$d')"
finish_test() {
  cleanup || true
  case "$test_dir" in /tmp/oai-monitor-test.*) rm -rf "$test_dir" ;; esac
}
trap finish_test EXIT
prepare_scheduler
gnb_fifo="$test_dir/gnb.fifo"
nrue_fifo="$test_dir/ue.fifo"
gnb_counter="$test_dir/count"
nrue_state="$test_dir/state"
start_gnb_monitor "$gnb_fifo" "$test_dir/gnb.log" "$gnb_counter"
start_nrue_monitor "$nrue_fifo" "$test_dir/ue.log" "$nrue_state"
# Keep both writers open so the collectors must publish before EOF. Fragment
# a marker across writes to catch read buffering and partial-line regressions.
exec 8>"$gnb_fifo" 9>"$nrue_fifo"
printf 'Frame.' >&8
sleep 1
[ "$(read_counter "$gnb_counter")" = 0 ] || die 'partial marker counted'
printf 'Slot 128.0\n' >&8
printf 'ordinary decode failed\nUE synchronized!\n' >&9
i=0
while [ "$i" -lt 80 ]; do
  printf 'ERROR diagnostic %s\n' "$i" >&8
  i=$((i + 1))
done
printf 'Frame.Slot 256.0\n' >&8
i=0
while [ "$(read_counter "$gnb_counter")" != 2 ]; do
  i=$((i + 1)); [ "$i" -lt 10 ] || die 'collector did not update before EOF'
  sleep 1
done
read -r state <"$nrue_state"
[ "$state" = SYNC ] || die 'recoverable decode failure was fatal'
lines=$(cat "$test_dir/gnb.log.prev" "$test_dir/gnb.log" | wc -l)
[ "$lines" -le 64 ] || die 'event retention exceeded bound'
echo "PASS: live FIFO, split marker, atomic counter, bounded events ($lines lines), recoverable UE error"
chrt -o 0 sleep 60 & gnb_pid=$!
chrt -o 0 sleep 60 & nrue_pid=$!
HEARTBEAT_INTERVAL=1
if wait_for_frame_markers "$gnb_counter" 2 1 "$gnb_pid" 2; then
  die 'stalled workload incorrectly passed'
else
  rc=$?; [ "$rc" = 1 ] || die "expected timeout, got $rc"
fi
echo 'PASS: no-progress deadline returns failure'
printf 'Frame.Slot 384.0\n' >&8
sleep 1
wait_for_frame_markers "$gnb_counter" 2 1 "$gnb_pid" 5 || die 'target not detected'
[ "$observed_frame_markers" = 1 ] || die 'wrong measurement delta'
echo 'PASS: target reached, workload frozen'
printf 'fatal error: injected test\n' >&9
sleep 1
if wait_for_sync_state "$nrue_state" "$nrue_pid" 5; then
  die 'fatal UE state ignored'
else
  rc=$?; [ "$rc" = 2 ] || die "expected fatal, got $rc"
fi
stop_monitor "$gnb_monitor_pid"
sleep 1
if wait_for_frame_markers "$gnb_counter" 3 1 "$gnb_pid" 5; then
  die 'dead collector ignored'
else
  rc=$?; [ "$rc" = 3 ] || die "expected collector exit, got $rc"
fi
gnb_monitor_pid=
echo 'PASS: fatal UE and collector death detected'
exec 8>&- 9>&-
cleanup || die 'stopped workload/collectors failed to exit'
echo 'PASS: stopped workload and live FIFO readers cleaned without wait'

# Exercise a real isolated workload group, including a forked helper.
cleanup_done=0
setsid chrt -o 0 /bin/busybox sh -c 'sleep 60 & echo $! >"$1"; wait' sh "$test_dir/child.pid" &
gnb_pid=$!
confirm_workload_group "$gnb_pid" || die 'setsid did not establish owned group'
gnb_pgid=$gnb_pid
i=0
while [ ! -s "$test_dir/child.pid" ]; do
  i=$((i + 1)); [ "$i" -le 5 ] || die 'helper PID was not published'
  sleep 1
done
read -r child_pid <"$test_dir/child.pid"
freeze_workload
cleanup || die 'isolated group cleanup failed'
if process_is_live "$child_pid"; then die 'forked helper survived cleanup'; fi
cleanup || die 'successful cleanup is not idempotent'
echo 'PASS: frozen leader and descendant terminated; repeated cleanup is safe'

# A kernel D-state cannot safely be created here. Inject persistent liveness,
# not a real stuck kernel task, and prove the deadline returns FAIL, not PASS.
(
  cleanup_done=0 cleanup_status=0 CLEANUP_TIMEOUT=2
  cleanup_pending() { pending='injected:uninterruptible'; return 0; }
  started=$(uptime_seconds)
  if cleanup; then die 'persistent liveness incorrectly passed'; fi
  elapsed=$(( $(uptime_seconds) - started ))
  [ "$elapsed" -ge 2 ] && [ "$elapsed" -lt 6 ] || die 'cleanup deadline not bounded'
  if cleanup; then die 'failed cleanup incorrectly passed on retry'; fi
)
echo 'PASS: injected stuck-task timeout returns FAIL and is not retried'
echo 'ALL MONITOR TESTS PASSED'
