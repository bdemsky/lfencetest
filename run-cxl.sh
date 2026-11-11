#!/bin/sh
# Build and run the CXL-TSO hardware checks on one P-core and one E-core
# (hybrid CPUs), plus the coherent control. See LITMUS.md.
#   litmus-cpu<n>.txt, litmus-coherent.txt   the paper's litmus tests (cxl_litmus)
#   out-cpu<n>.txt, out-coherent.txt         instruction-level checks  (cxl_test)
# Override the cores with PCORE=<n> ECORE=<n>. The second thread of the host
# runs on the next core (PCORE+1, ECORE+1).
set -e
cd "$(dirname "$0")"
make -s cxl_test cxl_litmus

first() { [ -r "$1" ] && sed 's/[-,].*//' "$1"; }
PCORE=${PCORE:-$(first /sys/devices/cpu_core/cpus)}
PCORE=${PCORE:-0}
ECORE=${ECORE:-$(first /sys/devices/cpu_atom/cpus)}

for c in $PCORE $ECORE; do
  CPU=$c ./cxl_litmus 100 > litmus-cpu$c.txt 2>&1 || true
  CPU=$c ./cxl_test 300 > out-cpu$c.txt 2>&1 || true
  tail -n 1 litmus-cpu$c.txt out-cpu$c.txt
done
CPU=$PCORE COHERENT=1 ./cxl_litmus 100 > litmus-coherent.txt 2>&1
CPU=$PCORE COHERENT=1 ./cxl_test 100 > out-coherent.txt 2>&1
tail -n 1 litmus-coherent.txt out-coherent.txt
