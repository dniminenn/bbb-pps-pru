#!/bin/bash
# /usr/local/bin/pps-rtprio.sh
# Called by pps-rtprio.service after chrony is up.
# Sets RT priorities for PPS timing stability on BeagleBone Black.

log() { logger -t pps-rtprio "$@"; }

# Match against /proc/*/comm which is the real 15-char truncated name
set_prio() {
    local prio=$1 pattern=$2
    local found=0
    for comm_file in /proc/[0-9]*/comm; do
        pid=${comm_file#/proc/}
        pid=${pid%/comm}
        if grep -q "^${pattern}$" "$comm_file" 2>/dev/null; then
            chrt -f -p "$prio" "$pid" && log "set $pattern (pid $pid) to FIFO:$prio"
            found=1
        fi
    done
    [ "$found" -eq 0 ] && log "WARN: $pattern not found"
}

drop_rt() {
    local pattern=$1
    for comm_file in /proc/[0-9]*/comm; do
        pid=${comm_file#/proc/}
        pid=${pid%/comm}
        if grep -q "^${pattern}$" "$comm_file" 2>/dev/null; then
            chrt -o -p 0 "$pid" && log "set $pattern (pid $pid) to SCHED_OTHER"
        fi
    done
}

sleep 5

# PPS GPIO bank — parent interrupt controller, must be highest
set_prio 91 "irq/23-4804c000"

# PPS IRQ thread
set_prio 90 "irq/54-pps.-1"

# HDMI/display — not timing critical
set_prio 10 "irq/45-tda998x"
set_prio 10 "irq/46-tilcdc"
set_prio 10 "card0-crtc0"

# MMC/SD
set_prio 20 "irq/47-mmc0"
set_prio 20 "irq/47-s-mmc0"
set_prio 20 "irq/28-mmc1"
set_prio 20 "irq/28-s-mmc1"
set_prio 20 "irq/49-48060000"

# USB
set_prio 20 "irq/48-musb-hdrc"
set_prio 20 "irq/35-musb-hdrc"

# Ethernet switch
set_prio 20 "irq/30-4a100000"
set_prio 20 "irq/31-4a100000"
set_prio 20 "irq/32-4a100000"

# chrony_exporter doesn't need RT
drop_rt "chrony_exporte"

log "priority tuning complete"
