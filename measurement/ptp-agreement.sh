#!/usr/bin/env bash
# On-demand readout of the PTP-measured time agreement between this box and the
# grandmaster it is watching (e.g. .23 vs .24). Prints nothing in the background;
# run it when you want a number.
#
# How it works: ptp4l runs free_running (measure only, never disciplines), and
# cpts-phc2sys keeps the CPTS PHC equal to this box's GPS-driven system clock in
# the TAI scale. So ptp4l's offsetFromMaster is (this GPS) minus (GM GPS) directly.
#
# cpsw hardware timestamping is noisy and positive-skewed (occasional us spikes),
# so a single sample is misleading. We sample N times and report the MEDIAN as the
# agreement, with the spread shown for context. No path-asymmetry correction is
# applied because the measured path is symmetric (median ~0); see measurement/README.
set -u
# Sample over a ~60s window by default. cpsw offset wanders +-15us over tens of
# seconds but centers near zero over a minute, so a longer window is needed for a
# stable median. ptp4l updates the offset about once per second, so space samples
# 2s apart to keep them independent.
N="${1:-30}"
PMC=(/usr/sbin/pmc -u -b 0)

if [ "$(id -u)" -ne 0 ]; then echo "run as root (pmc needs the ptp4l UDS)"; exit 2; fi

state=$("${PMC[@]}" "GET PORT_DATA_SET" 2>/dev/null | awk '/portState/{print $2}')
gm=$("${PMC[@]}" "GET PARENT_DATA_SET" 2>/dev/null | awk '/grandmasterIdentity/{print $2}')

tmp=$(mktemp)
for _ in $(seq 1 "$N"); do
  "${PMC[@]}" "GET CURRENT_DATA_SET" 2>/dev/null \
    | awk '/offsetFromMaster/{o=$2} /meanPathDelay/{d=$2} END{if(o!="")print o, d}' >> "$tmp"
  sleep 2
done

sort -n "$tmp" | awk -v st="${state:-?}" -v gm="${gm:-?}" '
  {o[n]=$1; sd+=$2; n++}
  END{
    if(n==0){print "no PTP samples - is ptp4l up and locked to the GM?"; exit 1}
    med=(n%2)?o[int(n/2)]:(o[int(n/2)-1]+o[int(n/2)])/2
    printf "PTP agreement vs grandmaster %s (port %s, %d samples)\n", gm, st, n
    printf "  median offset  : %+.0f ns    <- the agreement\n", med
    printf "  spread (min/max): %+.0f / %+.0f ns   (cpsw measurement noise)\n", o[0], o[n-1]
    printf "  mean path delay : %.0f ns\n", sd/n
    if (med>1000000000 || med<-1000000000)
      print "  WARNING: seconds-scale offset - cpts-phc2sys reference is down or CPTS not disciplined"
  }'
rm -f "$tmp"
