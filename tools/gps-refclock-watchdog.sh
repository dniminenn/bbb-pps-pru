#!/usr/bin/env bash
# Restart gpsd if chrony's GPS coarse refclock (gpsd -> SHM NTP0) has gone stale.
#
# Failure mode this guards against: gpsd can silently stop publishing the coarse
# time to SHM NTP0 while the gpsd process stays alive, so a plain Restart= policy
# never triggers. With NTP0 dead, chrony cannot resolve the integer second for the
# PPS refclock (lock GPS), discards the good PPS, and coasts (root dispersion
# climbs, the clock drifts). SHM NTP0 is written only by gpsd, so a GPS refclock
# with reach 0 is unambiguously a gpsd fault.
#
# The double read (12s apart) avoids a false restart during the normal reach
# rebuild right after gpsd starts, when reach legitimately reads 0 for a poll or two.
set -u

# A fresh chronyd or gpsd legitimately reads reach 0 while rebuilding; judging
# either inside its first 3 minutes restarts a healthy gpsd, which costs ~2 min
# of NTP0 silence and rings the clock (both PPS refclocks lock GPS).
now_us=$(awk '{printf "%d", $1*1000000}' /proc/uptime)
for u in chrony gpsd; do
  s=$(systemctl show "$u" -p ActiveEnterTimestampMonotonic --value)
  [ -n "$s" ] && [ "$s" -gt 0 ] && [ $((now_us - s)) -lt 180000000 ] && exit 0
done

gps_reach() { chronyc -n sources 2>/dev/null | awk '$2=="GPS"{print $5}'; }

r1=$(gps_reach)
[ "${r1:-x}" = "0" ] || exit 0   # healthy, rebuilding, or chrony down: nothing to do
sleep 12
r2=$(gps_reach)
[ "${r2:-x}" = "0" ] || exit 0   # cleared on its own (post-restart blip): done

logger -t gps-refclock-watchdog "GPS refclock (gpsd NTP0) reach=0 for >12s; restarting gpsd"
systemctl restart gpsd gpsd.socket
