#!/bin/busybox sh
# Test fixture only: generate progress without running radio algorithms.
case "$0" in
  */nr-softmodem)
    n=0
    while [ "$n" -lt 30 ]; do
      echo "Frame.Slot $((n * 128)).0"
      n=$((n + 1))
      sleep 1
    done
    ;;
  */nr-uesoftmodem)
    echo 'UE synchronized!'
    sleep 30
    ;;
  *) exit 2 ;;
esac
