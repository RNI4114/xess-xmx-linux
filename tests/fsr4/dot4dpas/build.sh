#!/bin/bash
# build.sh: generate the test shaders (gen.py: chains of 8-bit dot products against constant or uniform weights, with
# the CPU reference) and the runner. Then, per case:  VK_DRIVER_FILES=<icd> ./dot4dpas <case>   (INTEL_DOT4_DPAS=0|1|2)
set -e
cd "$(dirname "$0")"
python3 gen.py
for f in *.comp; do glslangValidator --target-env vulkan1.3 -o "${f%.comp}.spv" "$f" >/dev/null; done
cc -O2 -Wall -o dot4dpas dot4dpas.c -lvulkan
