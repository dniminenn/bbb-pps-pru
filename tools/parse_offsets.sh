#!/usr/bin/env bash
# parse_offsets.sh — extract offset values from pru_pps_shm daemon logs
# and compute basic statistics (min, max, mean, stddev).
#
# Usage: tools/parse_offsets.sh <logfile> [logfile2 ...]
#
# Input: journalctl or tee'd output from pru_pps_shm containing lines like:
#   seq=1234 delta=200011550 offset=+352 ns gap=18354 ...

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <logfile> [logfile2 ...]" >&2
    exit 1
fi

# Extract offset values (strip the sign-prefix for awk, keep sign in value)
grep -hEo 'offset=[+-]?[0-9]+' "$@" \
    | sed 's/offset=//' \
    | awk '
BEGIN {
    n = 0; sum = 0; sumsq = 0;
    min = 999999999; max = -999999999;
}
{
    v = $1 + 0;
    n++;
    sum += v;
    sumsq += v * v;
    if (v < min) min = v;
    if (v > max) max = v;
    # histogram buckets (100 ns wide)
    bucket = int(v / 100) * 100;
    hist[bucket]++;
}
END {
    if (n == 0) { print "No offset samples found."; exit 1; }
    mean = sum / n;
    variance = (sumsq / n) - (mean * mean);
    stddev = sqrt(variance > 0 ? variance : 0);

    printf "\n=== PRU PPS Offset Statistics ===\n";
    printf "Samples:  %d\n", n;
    printf "Min:      %+d ns\n", min;
    printf "Max:      %+d ns\n", max;
    printf "Mean:     %+.1f ns\n", mean;
    printf "Std Dev:  %.1f ns\n", stddev;
    printf "Range:    %d ns\n", max - min;

    printf "\n=== Histogram (100 ns buckets) ===\n";
    PROCINFO["sorted_in"] = "@ind_num_asc";
    for (b in hist) {
        bar = "";
        for (i = 0; i < hist[b] && i < 60; i++) bar = bar "#";
        if (hist[b] > 60) bar = bar "...";
        printf "  [%+5d ns] %4d  %s\n", b, hist[b], bar;
    }
}'
