#!/bin/sh
# Build and run the full test suite. Output goes to stdout and results.txt.
#
# Picks one P-core and one E-core automatically on hybrid Intel CPUs
# (override with PCORE=<n> ECORE=<n>). On non-hybrid CPUs only PCORE runs.
set -e
cd "$(dirname "$0")"
make -s

first() { [ -r "$1" ] && sed 's/[-,].*//' "$1"; }
PCORE=${PCORE:-$(first /sys/devices/cpu_core/cpus)}
PCORE=${PCORE:-2}
ECORE=${ECORE:-$(first /sys/devices/cpu_atom/cpus)}

{
  echo "# $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "# kernel $(uname -r), $(date -u '+%Y-%m-%d %H:%M UTC'), P-core=$PCORE E-core=${ECORE:-none}"
  echo
  echo "## gpu_stale: one trial at a time"
  for CPU in $PCORE $ECORE; do CPU=$CPU ./gpu_stale 2 200; echo; done
  echo "## controls: PAT 3 (coherent), PAT 0 (WB non-coherent), copy engine"
  CPU=$PCORE ./gpu_stale 3 100; echo
  CPU=$PCORE ./gpu_stale 0 100; echo
  CPU=$PCORE ./gpu_stale 2 100 blt; echo
  echo "## gpu_stale grouped: K x clflushopt, one fence, K loads"
  for CPU in $PCORE $ECORE; do
    for K in 1 4 16 64; do for F in 0 1 2; do CPU=$CPU K=$K F=$F ./gpu_stale 2 100; done; done
  done
  echo
  echo "## unrolled_test: back to back"
  for CPU in $PCORE $ECORE; do CPU=$CPU ./unrolled_test 1600; echo; done
} 2>&1 | tee results.txt
