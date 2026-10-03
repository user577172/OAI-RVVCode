#!/bin/busybox sh
# Validate production run_once result ordering and overall failure semantics.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_dir=$(mktemp -d /tmp/oai-report-test.XXXXXX)
finish() { case "$test_dir" in /tmp/oai-report-test.*) rm -rf "$test_dir" ;; esac; }
trap finish EXIT
mkdir -p "$test_dir/bundle/bin" "$test_dir/bundle/etc"
cp "$script_dir/tests/oai-monitor-fake.sh" "$test_dir/bundle/bin/nr-softmodem"
cp "$script_dir/tests/oai-monitor-fake.sh" "$test_dir/bundle/bin/nr-uesoftmodem"
chmod 755 "$test_dir/bundle/bin/"*
: >"$test_dir/bundle/etc/channelmod_rfsimu_LEO_satellite.conf"

run_case() (
  BUILD="$test_dir/bundle"
  GNB="$BUILD/bin/nr-softmodem" NRUE="$BUILD/bin/nr-uesoftmodem"
  GNB_CONF=/dev/null UE_CONF=/dev/null
  RESULTS_DIR="$test_dir/$1"
  mkdir -p "$RESULTS_DIR"
  WARMUP_SECONDS=1 MEASUREMENT_SECONDS=3 SYNC_TIMEOUT=10 GNB_READY_TIMEOUT=10
  FRAME_MARKER_INTERVAL=128 HEARTBEAT_INTERVAL=5 CLEANUP_TIMEOUT=2 OAI_KEEP_EVENTS=0
  GNB_THREAD_POOL=-1 UE_THREAD_POOL=-1,-1
  gnb_pid= nrue_pid= gnb_pgid= nrue_pgid= gnb_monitor_pid= nrue_monitor_pid=
  gnb_fifo= nrue_fifo= cleanup_done=0 cleanup_status=0 work_dir= gnb_events= nrue_events=
  die() { echo "ERROR: $*" >&2; exit 1; }
  eval "$(sed -n '/^read_process_stat() {/,/^last_run=/p' "$script_dir/run-xsai-fpga-performance.sh" | sed '$d')"
  port_is_listening() { return 0; }
  if [ "$1" = fail ]; then
    cleanup_pending() { pending='injected:stuck'; return 0; }
  fi
  run_once 1
  echo 'OAI_XSAI_PERF_RESULT=PASS'
)

check_order() {
  awk '
    /OAI_XSAI_CSV_BEGIN/ { b=NR }
    /OAI_XSAI_CSV_END/ { e=NR }
    /OAI_XSAI_MEASUREMENT_RESULT=PASS/ { m=NR }
    /OAI_XSAI_CLEANUP_RESULT=PASS/ { c=NR }
    END { exit !(c > 0 && c < b && b < e && e < m) }
  ' "$1"
}
if ! run_case pass >"$test_dir/pass.log" 2>&1; then
  cat "$test_dir/pass.log"; exit 1
fi
check_order "$test_dir/pass.log"
grep -q 'OAI_XSAI_CLEANUP_RESULT=PASS' "$test_dir/pass.log"
grep -q '^OAI_XSAI_PERF_RESULT=PASS' "$test_dir/pass.log"
[ "$(find "$test_dir/pass" -type f | wc -l)" -eq 1 ]
grep -qx 'result,PASS,status' "$test_dir/pass/oai-xsai-newwork.csv"
grep -qx 'warmup_target_seconds,1,seconds' "$test_dir/pass/oai-xsai-newwork.csv"
grep -qx 'measurement_target_seconds,3,seconds' "$test_dir/pass/oai-xsai-newwork.csv"
grep -qx 'module_stats_window_available,1,boolean' "$test_dir/pass/oai-xsai-newwork.csv"
grep -q '^gNB_feptx_total_window_avg_us,' "$test_dir/pass/oai-xsai-newwork.csv"
grep -q '^nrUE_OFDM_MOD_STATS_window_avg_us,' "$test_dir/pass/oai-xsai-newwork.csv"
echo 'PASS: production run retains only the CSV after successful cleanup'
if run_case fail >"$test_dir/fail.log" 2>&1; then
  cat "$test_dir/fail.log"; echo 'FAIL: cleanup fault returned success'; exit 1
fi
grep -q 'OAI_XSAI_CLEANUP_RESULT=FAIL' "$test_dir/fail.log"
if grep -q 'OAI_XSAI_PERF_RESULT=PASS' "$test_dir/fail.log"; then exit 1; fi
[ ! -e "$test_dir/fail/oai-xsai-newwork.csv" ]
echo 'PASS: cleanup fault returns nonzero without exporting a passing CSV'
echo 'ALL REPORT TESTS PASSED'
