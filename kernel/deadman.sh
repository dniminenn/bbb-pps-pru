#!/bin/sh
# load cpsw_pruts with auto-revert: dead-man rmmods on lost ping,
# panic_on_oops reboots to stock (module not persistent)
set -e
cd "$(dirname "$0")"
sysctl -w kernel.panic_on_oops=1 kernel.panic=10
systemd-run --collect --unit=pruts-deadman sh -c '
  sleep 15
  for i in 1 2 3 4; do
    ping -c2 -W2 192.168.0.1 >/dev/null 2>&1 && exit 0
    sleep 3
  done
  rmmod cpsw_pruts 2>/dev/null
  ip link set eth0 down; sleep 1; ip link set eth0 up
'
insmod ./cpsw_pruts.ko vlanif=eth0.71
