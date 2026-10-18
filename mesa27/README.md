# Mesa 27 (main) for xess-xmx + FSR4 on XMX

Patch series on top of Mesa main **`4c12390be30`** (2026-10-18), in parts, so each can be taken or left. This is the
driver the fs4-xmx work was tested with: applying everything below gives the exact tree of that build.

**Order:** first this repository's `patches/mesa-26.3/` (0001-0004: the XeSS C-for-Metal injection, its debug tools,
Large GRF around CM kernels and the divergent-barrier crash fix), which apply to Mesa main as they are; then the parts
here, in their numeric order. Every part applies on the ones before it; the XeSS-only driver is `patches/mesa-26.3`
plus, optionally, `1-xess-extra`.

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git && cd mesa && git checkout 4c12390be30
git am ../xess-xmx-linux/patches/mesa-26.3/*.patch
for p in 1-xess-extra 2-jay 3-rt-bvh 4-anv 5-fsr4-coopmat 6-fsr4-jay; do git am ../xess-xmx-linux/mesa27/$p/*.patch; done
meson setup build -Dbuildtype=release -Dprefix=/usr -Dsysconfdir=/etc -Db_ndebug=true -Dvulkan-drivers=intel \
    -Dgallium-drivers= -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
    -Dllvm=enabled -Dintel-rt=enabled -Dvideo-codecs= -Dvulkan-layers= -Dtools=
ninja -C build src/intel/vulkan/libvulkan_intel.so
```

The prefix matters: the driver reads its per-game workarounds from `/usr/share/drirc.d`.

## 1-xess-extra (2)

Two additions to the XeSS C-for-Metal injection that `patches/mesa-26.3` does not have yet: the general state pool initialised lazily for CM kernels under direct descriptors, and Large GRF Mode switched off again before trace rays and internal compute.

- anv: lazily init general_state_pool for CM kernels under direct descriptors
- anv/xe2: switch CM Large GRF Mode off before trace rays and internal compute

## 2-jay (10)

Jay fixes and small optimisations not in Mesa main yet (one of them is an upstream Intel fix that has not been merged), and a last commit that keeps two places of main the replay onto 4c12390be30 would otherwise lose.

- jay: Fix a SWSB issue with nested loops
- jay: accumulator pair fix in the scoreboard (WIP from xess-xmx-jayfix, committed for the RT series)
- jay: halt in a divergent loop body reconverges physically at the loop exit
- jay: LSC local fence before trace_ray/BTD messages, like brw
- jay: pick the least recently used SBID when all are busy
- jay: Fix conflict between some RA temporary registers
- jay: bake cross-pipe RegDist into in-order instructions on Xe-HP+
- jay: resolve parallel copy cycles by rotation instead of swaps
- jay: vectorize post-RA splats of the same immediate into consecutive UGPRs
- jay: rebase onto Mesa main 4c12390be30: scoreboard and BTD SIMD width as merged

## 3-rt-bvh (21)

The ray tracing / BVH build series (H-PLOC, refit, morton sort, batching, fewer device-scope fences) and two NIR fixes it needed. Written for performance on Xe2.

- anv/bvh: batch update/header dispatches; fix OOB shared store of update transform
- anv/bvh: refit without per-level L1 clean/invalidate (coherent scratch + read-back ordering)
- anv/bvh: encode without device-scope fences in the spin loop (coherent bvh_offset)
- anv/bvh: use H-PLOC instead of PLOC (TLAS and BLAS); handle HPLOC debug marker
- anv/bvh: don't mutate device-wide build args in encode; ANV_ENCODE_FENCES=1 switch to upstream encode
- anv: CPU trace markers on AS build/copy
- vk/anv: CPU trace markers on BVH build stages
- vulkan/bvh: morton sort: spin on a relaxed RMW instead of an acquire load
- vulkan/bvh: H-PLOC: coherent publish instead of device barriers, one node allocation per cluster
- vulkan/bvh: H-PLOC: load the neighbor bounds in batches
- vulkan/bvh: morton sort: merge-path searches on two subgroups in parallel
- anv/bvh: size parent_child_count_map per block, not per internal node
- anv/bvh: batch the scratch initializations of a build call in one dispatch
- vulkan/bvh: lbvh_generate_ir without per-level device-scope fences
- anv/bvh: refit: unroll the per-child loops of recompute_parent
- vulkan/bvh, anv: separate morton sort width for 64-bit keys (TLAS)
- anv/bvh: refit: load the read-only maps of a level together
- vulkan/bvh, anv: wide morton sort for big BLAS too
- anv/bvh: write the real instance mask into TLAS internal nodes
- nir/liveness: account for the following if condition in nir_get_live_defs()
- nir/lower_shader_calls: make scratch_size the largest call frame

## 4-anv (10)

ANV: dumps for debugging, utrace, slab allocation, markers, a narrower L1/L2 flush; the cross-queue wait experiment is opt-in (`ANV_XQ`).

- anv: one shader-dump archive per device
- anv/utrace: skip copy/video queues (compute copy fallback hangs bcs)
- anv: map slab parents once instead of mmap/munmap per slab entry
- anv: don't drain the pipeline for top-of-pipe buffer markers
- anv/utrace: don't read the batch back when adding a walker post-sync timestamp
- anv: keep empty slab parent BOs around instead of freeing them
- anv/xe: resolve queue-to-queue timeline waits without the kernel (experimental)
- anv/xe: make the cross-queue wait experiment (ANV_XQ) opt-in
- anv/xe2: only flush L1/L2 for a MEMORY_WRITE destination access
- anv: dump every kernel of RT binaries, replay copies and internal kernels

## 5-fsr4-coopmat (3)

Cooperative matrices 16x16 in wave32 (two stacked 8-row DPAS), the shape D3D12 WMMA and so FSR4's FP8 model need; widened slices for 8-bit types; Large GRF for DPAS shaders.

- dlss-jay: coopmat 16x16x16 split, vector/block cmat loads, Jay phi RA affinity, Large GRF for DPAS, ANV_CM_OFF
- intel: 16x16 cooperative matrices for D3D12 WMMA (FSR4) in wave32
- intel: rebase onto Mesa main 4c12390be30: keep main's RA stride affinity and cmat conversion signedness

## 6-fsr4-jay (9)

Jay: chains of 8-bit dot products against constant (or reused uniform) weights turned into XMX DPAS (FSR4's INT8 model; `INTEL_DOT4_DPAS=0` off), DPAS reading A in place from UGPRs, UGPR room for whole-GRF vectors, optional fp16->fp32 for compute (`INTEL_PROMOTE_F16=1`, off by default), INTEL_MEASURE fixes, and two correctness fixes: no immediates in DP4A (a 16-bit immediate gave wrong results for weights that fit in 16 bits, in any game) and no DPAS where an empty branch forwards values.

- jay: halt's physical edge to the loop exit goes first in the successors
- intel/measure: keep the batch queue sound with several queues and reused command buffers
- jay: let DPAS read A in place from a subgroup block read
- jay: turn chains of 8-bit dot products against constant weights into DPAS
- jay: leave UGPR room for whole-GRF vectors
- jay: optionally do half float compute math in 32 bits
- jay: dot4->DPAS with uniform weights, when they are reused
- jay: dot4->DPAS, no DPAS where an empty branch forwards values
- jay: no immediates in DP4A

## Tests

`tests/fsr4/` (synthetic, nothing from FSR4): `dot4dpas` checks the dot product rewrite bit for bit against a CPU
reference (constant and uniform weights, partial subgroups, non-uniform control flow, 16-bit weights), `wmma32` the 16x16
cooperative matrices at subgroup 16 and 32. See docs/fsr4.md.
