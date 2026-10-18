# FSR4 on XMX (Intel Xe2)

*This builds on HHHHanasak1's xess-xmx-linux, which made the XMX path possible: the igdext shim, the C-for-Metal kernel injection in ANV and the packaging. The FSR4 work is an addition on top of it and reuses its driver patches unchanged (`patches/mesa-26.3`).*

AMD's FSR4 (4.1.1) runs on an Arc B580 through Proton, the way AMD's driver offers it on Windows: games with FSR 3.1
get FSR4 in their own upscaler menu; games without it get it through OptiScaler. Its INT8 model runs on the XMX units
(DPAS), its FP8 model on cooperative matrices. This branch has the driver, the Proton-side patches, a small proxy DLL
and a launcher. **Nothing from AMD is here**: FSR4's DLL comes from the user, and no weights or shaders of it are in
this repository.

## What it takes

| piece | where | what it does |
|---|---|---|
| Mesa 27 driver | `mesa27/` (on top of `patches/mesa-26.3`) | 16x16 wave32 cooperative matrices (FP8 model), 8-bit dot products against constant weights as DPAS (INT8 model), Jay fixes |
| vkd3d-proton | `fsr4/vkd3d-proton/` | `VKD3D_XMX_WMMA=1` exposes D3D12 wave matrices (WMMA) on ANV and loads the `amdxc64` shim there |
| dxil-spirv | `fsr4/dxil-spirv/` | the accumulator layout fixup of `wmma_rdna3_workaround` on its own (`DXIL_SPIRV_CONFIG=wmma_rdna3_layout`) |
| Proton | a build with the two patches above and an `amdxc64` shim (wineland's) | `amdxc64` answers FSR4's AGS queries; it reports the emulated FP8 wave matrices only with `DXIL_SPIRV_CONFIG=wmma_rdna3_workaround` |
| fs4shim | `fsr4/fs4shim/` (`make`, mingw-w64) | see below |
| launcher | `fsr4/fsr4-launch.sh` | stages the DLLs beside the game's exe for one session and sets the environment |

FSR4 4.1.1's `amd_fidelityfx_upscaler_dx12.dll`, the FidelityFX SDK 2.3 loader (`amd_fidelityfx_dx12.dll`) and OptiScaler
are taken from goverlay's downloads by default (`~/.local/share/goverlay/...`); `FS4_UPSCALER_DLL`, `FS4_FFX_LOADER`
and `FS4_OPTISCALER` point elsewhere.

## Use

Build the Mesa 27 driver (`mesa27/README.md`) and put it next to the package's others as
`lib/mesa-27/libvulkan_intel.so`, so the ICD picks it per game with `MESA=27` (see the README); build `fsr4/fs4shim`
(`make`); and put into the game's launch options:

    MESA=27 /path/to/xess-xmx-linux/fsr4/fsr4-launch.sh %command%

XeSS keeps working with that driver (it carries the package's patches), so the same game can switch between XeSS and
FSR4 in its own menu.

- **Game with FSR 3.1** (it ships its own `amd_fidelityfx_dx12.dll`): pick FSR4 in its menu. The launcher puts the
  SDK 2.3 loader, fs4shim and FSR4 4.1.1 beside the exe.
- **Game without FSR 3.1**: OptiScaler takes the game's DLSS (when it ships `nvngx_dlss.dll`; the GPU is then reported
  as NVIDIA through OptiScaler's DXGI spoofing and fakenvapi so the game offers DLSS), else its XeSS, else FSR2/3, and
  runs FSR4. Pick that upscaler in the game's menu. `FS4_INPUTS=dlss|xess|ffx` forces one.
- The game folder is put back at the next launch (Steam stops the script with the game); by hand:
  `FS4_GAME_DIR=<exe folder> fsr4-launch.sh --restore`.
- `FS4_MODEL=int8` (default) or `fp8`; `FS4_OFF=1` stages nothing; `FS4_OPTIDEBUG=1` shows FSR4's watermark and logs
  OptiScaler at debug level. Logs and per-session settings: `~/.local/share/fs4-xmx/`.

AMD's own FSR4 provider (`amdxcffx64.dll`, which wineland's protonfixes install) runs no network on a non-AMD GPU; the
launcher keeps it from loading (`amdxcffx64=d`).

## fs4shim

FSR4's DLL lists FSR4 only when `ffxProvider_FSR4::IsSupported` says yes, and that reads a description of the GPU
from the AMD kernel driver's private adapter data (`D3DKMTQueryAdapterInfo`), which only exists on Windows with an AMD
driver. fs4shim is a proxy `amd_fidelityfx_upscaler_dx12.dll` that loads the real one (as
`amd_fidelityfx_upscaler_dx12_fsr4.dll`), finds the provider's vtable through its RTTI and replaces that check with the
DLL's own wave-matrix check (FP8) or lets the INT8 provider run on any device. It changes nothing on disk, contains no
AMD code, and if the DLL is not the version it was written for (4.1.1) it patches nothing. AMD's license for the DLL
may not allow this; decide for yourself.

## Numbers (Cyberpunk 2077 benchmark, 2560x1440, FSR "Performance" = 1280x720, Arc B580)

| | fps |
|---|---|
| FSR4 FP8 (cooperative matrices, FP8 emulated) | 55 |
| FSR4 INT8, dot products on the vector units | 72 |
| FSR4 INT8 with the DPAS rewrite | 78-80 |
| XeSS 2.0 "Balanced" (same 1280x720) | 89 |

FSR4's passes cost about 2.7 ms of GPU per frame against about 1.3 ms for XeSS: the network on the XMX is a small part of
it, the rest is code written for RDNA (fp16 packing, requantization between layers, loops) running translated
(DXIL -> SPIR-V -> NIR -> Jay). In Wuthering Waves through OptiScaler: FSR4 2.4 ms, XeSS 1.1 ms of upscaler time.

Note on presets: the same name is not the same resolution. XeSS 2.x uses Performance 2.3x, Balanced 2.0x, Quality
1.7x; FSR uses Performance 2.0x, Balanced 1.7x, Quality 1.5x. Compare FSR4 Performance with XeSS Balanced.

## Found on the way (fixed in `mesa27/6-fsr4-jay`)

- Jay emitted DP4A with 16-bit immediates; a dot product against a constant that fits in 16 bits came out wrong (any
  game). brw never does this.
- The dot product rewrite wrote DPAS results (NoMask) in non-uniform blocks whose empty `else` still forwards values to
  the merge, clobbering them for the inactive invocations.

## Not here

Rewrites of FSR4's own passes for Xe2 (one of them halves the cost of the preprocessing pass) are derived from AMD's
shaders and weights and stay private. The pieces to make them (a RenderDoc capture, a harness that runs one compute
shader on captured data, and vkd3d-proton's `VKD3D_SHADER_OVERRIDE`) are ordinary tools.
