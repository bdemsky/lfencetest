#!/bin/sh
# Build and run lfence_test on one P-core and one E-core (hybrid CPUs), plus
# the coherent control. Output goes to stdout and results.txt.
# Override the cores with PCORE=<n> ECORE=<n>; ROUNDS=<n> sets the rounds.
set -e
cd "$(dirname "$0")"
make -s

first() { [ -r "$1" ] && sed 's/[-,].*//' "$1"; }
PCORE=${PCORE:-$(first /sys/devices/cpu_core/cpus)}
PCORE=${PCORE:-2}
ECORE=${ECORE:-$(first /sys/devices/cpu_atom/cpus)}
ROUNDS=${ROUNDS:-300}

{
  echo "# $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "# kernel $(uname -r), $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo
  for c in $PCORE $ECORE; do
    CPU=$c ./lfence_test "$ROUNDS"
    echo
  done
  CPU=$PCORE COHERENT=1 ./lfence_test 100
} 2>&1 | tee results.txt
