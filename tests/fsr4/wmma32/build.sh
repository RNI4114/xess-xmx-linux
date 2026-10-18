#!/bin/bash
# build.sh: compile the test shader variants and the runner.
set -e
cd "$(dirname "$0")"
g() { glslangValidator --target-env vulkan1.3 -I. -o "$1.spv" "${@:3}" "$2" >/dev/null; }
g mma mma.comp; g mma_bcm mma.comp -DBCM; g mma_dcm mma.comp -DDCM; g mma_bcm_dcm mma.comp -DBCM -DDCM
for v in "" ACM BCM OCM "ACM BCM OCM"; do
  n=fp8emu; d=""; for x in $v; do n="${n}_$(echo $x | tr A-Z a-z)"; d="$d -D$x"; done
  g $n fp8emu.comp $d
done
g acc8 acc8.comp; g acc8_icm acc8.comp -DICM
cc -O2 -Wall -o wmma32 wmma32.c -lvulkan -lm
