#!/bin/busybox sh
# Test fixture only: generate progress without running radio algorithms.
case "$0" in
  */nr-softmodem)
    n=0
    while [ "$n" -lt 30 ]; do
      printf 'feptx_total: 100 us; %d; 200 us\n' "$n" >nrL1_stats.log
      echo "Frame.Slot $((n * 128)).0"
      n=$((n + 1))
      sleep 1
    done
    ;;
  */nr-uesoftmodem)
    echo 'UE synchronized!'
    n=0
    while [ "$n" -lt 30 ]; do
      printf 'OFDM_MOD_STATS: 40 us; %d; 80 us\n' "$n" >nrL1_UE_stats-0.log
      n=$((n + 1))
      sleep 1
    done
    ;;
  *) exit 2 ;;
esac
