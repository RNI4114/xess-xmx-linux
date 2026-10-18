#!/usr/bin/env bash
# fsr4-launch.sh — run a DX12 game with AMD FSR4 (4.1.1) on Intel Xe2 (XMX), like AMD's driver does on Windows: games
# with FSR 3.1 get FSR4 in their own menu; games without it get it through OptiScaler. Needs the Mesa 27 driver of
# this branch (mesa27/), Proton with the vkd3d-proton / dxil-spirv patches in fsr4/, and FSR4's DLL from the user (see
# docs/fsr4.md). Nothing from AMD is in this repository.
#
#   Steam / Heroic / Lutris launch options:  /path/to/xess-xmx-linux/fsr4/fsr4-launch.sh %command%
#   Put the game folder back by hand (e.g. after a crash):  FS4_GAME_DIR=<exe folder> fsr4-launch.sh --restore
#
# Modes (FS4_MODE), both staged beside the game's exe for the session only (put back at the next launch, since
# Steam stops this script with the game). Default auto: native when the game ships amd_fidelityfx_dx12.dll (FSR 3.1+
# through the FidelityFX API), otherwise optiscaler.
#   native            for games with FSR 3.1 / FSR4 (FidelityFX API): the FidelityFX SDK 2.3 loader in place of the
#                     game's amd_fidelityfx_dx12.dll, and the FSR4 4.1.1 upscaler DLL beside it behind fs4shim
#                     (fsr4/fs4shim). Pick FSR4 in the game's menu. Like xess-xmx: the game's own upscaler
#                     path, just pointed at what runs here.
#   optiscaler        for games without it: OptiScaler (as dxgi.dll) takes the game's DLSS / XeSS / FFX inputs
#                     (FS4_INPUTS, default auto: dlss when the game ships nvngx_dlss.dll, else xess when it ships
#                     libxess.dll, else ffx) and runs FSR4; with dlss the GPU is reported as NVIDIA (OptiScaler's DXGI
#                     spoofing + fakenvapi as nvapi64.dll) so the game offers DLSS. fs4shim stands in front of the FSR4
#                     DLL here too. Pick DLSS / XeSS / FSR in the game's menu.
# In both, the AMD driver's FSR4 provider (amdxcffx64.dll, which wineland's protonfixes put in system32 on every
# launch) is kept from loading: on a non-AMD GPU it runs no network at all (watermark "SOURCE: DRIVER", no WMMA or
# INT8 shader compiled), and it would win over the 4.1.1 DLL.
#
# The chain, all in fs4-xmx:
#   FSR4 DLL -> D3D12 WMMA (AGS) -> vkd3d-proton (Proton-Arc-VKD3D-Custom, VKD3D_XMX_WMMA=1) -> dxil-spirv
#   (FP8 kept as u8, math in FP16; DXIL_SPIRV_CONFIG=wmma_rdna3_workaround) -> VK_KHR_cooperative_matrix 16x16 wave32
#   -> the Mesa 27 ANV of this branch (mesa27/; Jay, two DPAS per 16x16x16).
# The same driver keeps XeSS on XMX working, so XeSS sessions can use it too.
#
# FS4_GAME_DIR   folder of the game's exe (default: the folder of the first .exe in the command)
# FS4_MODE       auto (default), native or optiscaler, see above
# FS4_MODEL      int8 (default: FSR4's INT8 model; the driver turns its 8-bit dot products into XMX DPAS, 77.8 fps in
#                the Cyberpunk benchmark) or fp8 (the FP8 model on the XMX wave matrices, emulated: 55 fps)
# FS4_INT8       (optiscaler mode) 1/0 = INT8/FP8 model; default follows FS4_MODEL
# INTEL_PROMOTE_F16 (default 0) the driver does half-precision compute math in fp32 — for every compute shader of the
#                game, so off: with it on, sunlit edges got purple dots at the end of the Cyberpunk benchmark (test 23,
#                gone with it off, test 24). The native 0055 does not need it.
# FS4_OFF=1      stage nothing and leave the WMMA variables out (same driver): the session without FSR4 (same driver)
# FS4_CAPTURE=1  record the game's pipelines (Fossilize) for offline replay against other driver builds
# FS4_TIMELINE=1 vkd3d queue timeline (waits, presents, PSO compiles) as a Chrome trace in $LOG/timeline.json
# FS4_MEASURE=1  GPU time per draw/dispatch (INTEL_MEASURE) on demand: echo <frames> > $FS4_HOME/measure.fifo
# FS4_WMMA=0     (INT8 only) do not expose D3D12 wave matrices: FSR4 then picks another set of INT8 kernels (the
#                one RenderDoc sessions get, as RenderDoc 1.46 has no VK_KHR_cooperative_matrix)
# FS4_OPTIDEBUG=1 (optiscaler mode) FSR4 watermark on screen and OptiScaler's log at debug level, nothing else
# FS4_RENDERDOC=1 RenderDoc's Vulkan layer (pacman renderdoc): F12 / PrtScr in game, or qrenderdoc -> File -> Attach
#                to Running Instance -> Trigger Capture; captures in $LOG/rdc/. Cooperative matrices are not captured by
#                RenderDoc, so use FS4_MODEL=int8 (the default).
# FS4_DIAG=1     FSR4 watermark on screen (optiscaler mode) + every shader vkd3d compiles dumped to $LOG/shaders (vkd3d disk cache
#                off). Count them afterwards, not here: Steam waits for this script before it stops the game.
# These can also go in $FS4_HOME/session.env (sourced at start).
# Logs: $FS4_HOME/logs/<date>/ (OptiScaler.log, vkd3d.log, steam-<appid>.log, staged.txt, session.txt).
set -u

PKG="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
D="${FS4_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/fs4-xmx}"   # logs, session.env
mkdir -p "$D"
# per-session settings (FS4_OFF, FS4_MODEL, FS4_INPUTS, ...) without touching the launch options
if [[ -f "$D/session.env" ]]; then
    set -a; source "$D/session.env"; set +a
fi
# Files from elsewhere (goverlay's downloads by default): OptiScaler, FSR4 4.1.1's upscaler DLL, the FidelityFX SDK 2.3
# loader. fs4shim is built from fsr4/fs4shim (make).
OPTI="${FS4_OPTISCALER:-$HOME/.local/share/goverlay/optiscaler-stable}"
FSR4_DLL="${FS4_UPSCALER_DLL:-$HOME/.local/share/goverlay/FSR4/Latest/amd_fidelityfx_upscaler_dx12.dll}"   # 4.1.1
FFX_LOADER="${FS4_FFX_LOADER:-$OPTI/amd_fidelityfx_dx12.dll}"   # FidelityFX SDK 2.3.0 loader (pairs with 4.1.1)
FS4SHIM="$PKG/fsr4/fs4shim/amd_fidelityfx_upscaler_dx12.dll"   # proxy: FSR4 offered on wave matrices alone

# The folder of the exe that creates the D3D12 device. Steam often starts a launcher instead (Cyberpunk:
# REDprelauncher.exe in the root, the game in bin/x64; Unreal: <Game>.exe in the root, the game in
# <Game>/Binaries/Win64/*-Shipping.exe), so look past those.
GAME_DIR="${FS4_GAME_DIR:-}"
if [[ -z "$GAME_DIR" ]]; then
    for arg in "$@"; do
        if [[ "$arg" == *.exe && -f "$arg" ]]; then
            GAME_DIR="$(dirname "$arg")"
            ROOT_DIR="$GAME_DIR"
            if compgen -G "$GAME_DIR/bin/x64/*.exe" >/dev/null; then
                GAME_DIR="$GAME_DIR/bin/x64"
            else
                ship=$(find "$GAME_DIR" -maxdepth 4 -name '*-Win64-Shipping.exe' -print -quit 2>/dev/null)
                [[ -n "$ship" ]] && GAME_DIR="$(dirname "$ship")"
            fi
            break
        fi
    done
fi
ROOT_DIR="${ROOT_DIR:-$GAME_DIR}"
STATE="$GAME_DIR/.fs4-xmx-staged"
BACKUP="$GAME_DIR/.fs4-xmx-backup"
# Earlier versions hid the prefix's amdxcffx64.dll here (HIDDEN lists files to put back); kept for the restore.
HIDDEN="$GAME_DIR/.fs4-xmx-hidden"

restore() {
    if [[ -n "$GAME_DIR" && -f "$HIDDEN" ]]; then
        while IFS= read -r f; do
            [[ -n "$f" && -f "$f.fs4-xmx-hidden" ]] && mv -f "$f.fs4-xmx-hidden" "$f"
        done < "$HIDDEN"
        rm -f "$HIDDEN"
    fi
    [[ -n "$GAME_DIR" && -f "$STATE" ]] || return 0
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        rm -f "$GAME_DIR/$f"
        [[ -f "$BACKUP/$f" ]] && mv -f "$BACKUP/$f" "$GAME_DIR/$f"
    done < "$STATE"
    rm -f "$STATE"
    rmdir "$BACKUP" 2>/dev/null || true
    echo "fsr4-launch: game folder restored ($GAME_DIR)" >&2
}

if [[ "${1:-}" == --restore ]]; then
    restore
    exit 0
fi
if [[ -z "$GAME_DIR" || ! -d "$GAME_DIR" ]]; then
    echo "fsr4-launch: game folder not found (set FS4_GAME_DIR); running without FSR4-XMX" >&2
    exec "$@"
fi

# a previous session that died without cleaning up
restore

LOG="$D/logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOG" "$BACKUP"
# keep the last 15 sessions (captures make them large)
ls -1dt "$D"/logs/*/ 2>/dev/null | tail -n +16 | xargs -r rm -rf
w() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"; }
stage() {   # stage SRC NAME
    local src=$1 name=$2
    [[ -f "$src" ]] || { echo "fsr4-launch: missing $src" >&2; return 1; }
    [[ -e "$GAME_DIR/$name" ]] && mv -f "$GAME_DIR/$name" "$BACKUP/$name"
    cp -f "$src" "$GAME_DIR/$name"
    echo "$name" >> "$STATE"
}
trap restore EXIT

MODE="${FS4_MODE:-auto}"
# auto: native when the game ships its own FidelityFX API loader. A copy of OptiScaler's / goverlay's (left behind by an
# earlier OptiScaler install, as in Wuthering Waves) does not count: the game itself does not call FFX then.
if [[ "$MODE" == auto ]]; then
    MODE=optiscaler
    if [[ -f "$GAME_DIR/amd_fidelityfx_dx12.dll" ]]; then
        own=$(md5sum < "$GAME_DIR/amd_fidelityfx_dx12.dll" | cut -c1-32)
        theirs=$(for f in "$OPTI"/amd_fidelityfx_dx12.dll "$HOME"/.local/share/goverlay/gameconfig/*/amd_fidelityfx_dx12.dll; do
                     [[ -f "$f" ]] && md5sum < "$f" | cut -c1-32; done)
        grep -qx "$own" <<< "$theirs" || MODE=native
    fi
fi
FS4_MODEL="${FS4_MODEL:-int8}"
FS4_INT8="${FS4_INT8:-$([[ $FS4_MODEL == int8 ]] && echo 1 || echo 0)}"
export INTEL_PROMOTE_F16="${INTEL_PROMOTE_F16:-0}"
if [[ "${FS4_OFF:-0}" != 1 && "$MODE" == native ]]; then
    # The upscaler DLL only offers FSR4 when the AMD kernel driver describes the GPU; fs4shim stands in front of it
    # (as amd_fidelityfx_upscaler_dx12.dll, the real one beside it as ..._fsr4.dll) and asks only for the wave
    # matrices, like the context creation does.
    stage "$FFX_LOADER" amd_fidelityfx_dx12.dll && stage "$FS4SHIM" amd_fidelityfx_upscaler_dx12.dll &&
        stage "$FSR4_DLL" amd_fidelityfx_upscaler_dx12_fsr4.dll ||
        { echo "fsr4-launch: staging failed; running without FSR4-XMX" >&2; trap - EXIT; restore; exec "$@"; }
    cp -f "$STATE" "$LOG/staged.txt"
elif [[ "${FS4_OFF:-0}" != 1 ]]; then
# OptiScaler.ini: the game's FFX (FSR 3.1) calls -> FSR4 upscaler; nothing else hooked, no GPU spoofing.
# Fsr4Update stays off: it routes FSR 3.1 to the AMD driver's FSR4 provider (amdxcffx64.dll, which wineland's
# protonfixes puts in system32 on every launch), and on a non-AMD GPU that provider runs no network at all
# (watermark "SOURCE: DRIVER", no WMMA or INT8 shader compiled). Off, OptiScaler's FSR 3.1 path takes the FSR4
# upscaler DLL staged beside the exe (4.1.1: FP8 provider on WMMA, INT8 provider), UpscalerIndex 0 = FSR4.
INI="$LOG/OptiScaler.ini"
INPUTS="${FS4_INPUTS:-auto}"
if [[ "$INPUTS" == auto ]]; then
    if [[ -n "$(find "$ROOT_DIR" -maxdepth 9 -iname nvngx_dlss.dll -print -quit 2>/dev/null)" ]]; then INPUTS=dlss
    elif [[ -f "$GAME_DIR/libxess.dll" ]]; then INPUTS=xess
    else INPUTS=ffx; fi
fi
python3 - "$OPTI/OptiScaler.ini" "$INI" "$(w "$LOG/OptiScaler.log")" "${FS4_INT8:-0}" "$([[ ${FS4_DIAG:-0} == 1 || ${FS4_OPTIDEBUG:-0} == 1 ]] && echo 1 || echo 0)" "$INPUTS" <<'PY'
import re, sys
src, dst, logfile, int8, diag, inputs = sys.argv[1:7]
on = lambda x: 'true' if inputs == x else 'false'
want = {('Upscalers', 'Dx12Upscaler'): 'fsr31',
        ('FSR', 'UpscalerIndex'): '0', ('FSR', 'Fsr4Update'): 'false',
        ('FSR', 'Fsr4ForceEnableInt8'): 'true' if int8 == '1' else 'false',
        ('Inputs', 'EnableFfxInputs'): on('ffx'), ('Inputs', 'UseFfxInputs'): on('ffx'),
        ('Inputs', 'EnableDlssInputs'): on('dlss'), ('Inputs', 'EnableXeSSInputs'): on('xess'),
        ('Spoofing', 'Dxgi'): on('dlss'),
        ('Log', 'LogToFile'): 'true', ('Log', 'LogLevel'): '0' if diag == '1' else '1', ('Log', 'LogFileName'): logfile}
if diag == '1':
    want[('FSR', 'Fsr4EnableWatermark')] = 'true'   # FSR4 draws its watermark when it really runs
out, section, seen = [], None, set()
for line in open(src, encoding='utf-8', errors='replace').read().splitlines():
    m = re.match(r'\s*\[([^\]]+)\]', line)
    if m:
        section = m.group(1)
    else:
        m = re.match(r'\s*([A-Za-z0-9_]+)\s*=', line)
        if m and (section, m.group(1)) in want and (section, m.group(1)) not in seen:
            line = f'{m.group(1)}={want[(section, m.group(1))]}'
            seen.add((section, m.group(1)))
    out.append(line)
missing = set(want) - seen
if missing:
    sys.exit(f'OptiScaler.ini lacks {sorted(missing)}')
open(dst, 'w', newline='').write('\r\n'.join(out) + '\r\n')   # CRLF like the original: OptiScaler ignores an LF-only ini
PY
[[ $? -eq 0 ]] || { echo "fsr4-launch: could not write OptiScaler.ini; running without FSR4-XMX" >&2; trap - EXIT; restore; exec "$@"; }
stage "$OPTI/OptiScaler.dll" dxgi.dll && stage "$INI" OptiScaler.ini &&
    stage "$FS4SHIM" amd_fidelityfx_upscaler_dx12.dll && stage "$FSR4_DLL" amd_fidelityfx_upscaler_dx12_fsr4.dll &&
    { [[ "$INPUTS" != dlss ]] || { stage "$OPTI/fakenvapi.dll" nvapi64.dll && stage "$OPTI/fakenvapi.ini" fakenvapi.ini; }; } ||
    { echo "fsr4-launch: staging failed; running without FSR4-XMX" >&2; trap - EXIT; restore; exec "$@"; }
cp -f "$STATE" "$LOG/staged.txt"
fi

# The driver is the package's (environment.d / MESA= in the launch options); this script only stages FSR4.
# FS4_MEASURE=1: ANV's INTEL_MEASURE (GPU time of every draw/dispatch) into $LOG/measure.csv, started by writing a
# frame count to $FS4_HOME/measure.fifo while the game runs (echo 200 > $FS4_HOME/measure.fifo)
if [[ "${FS4_MEASURE:-0}" == 1 ]]; then
    [[ -p "$D/measure.fifo" ]] || { rm -f "$D/measure.fifo"; mkfifo "$D/measure.fifo"; }
    export INTEL_MEASURE="draw,file=$LOG/measure.csv,control=$D/measure.fifo"
fi
# FS4_TIMELINE=1: vkd3d-proton's queue timeline (submissions, fence/event waits, presents, PSO compiles) as a
# Chrome trace in $LOG/timeline.json (open in chrome://tracing or ui.perfetto.dev)
if [[ "${FS4_TIMELINE:-0}" == 1 ]]; then
    export VKD3D_QUEUE_PROFILE="$(w "$LOG/timeline.json")" VKD3D_QUEUE_PROFILE_ABSOLUTE=1
fi
if [[ "${FS4_RENDERDOC:-0}" == 1 ]]; then
    mkdir -p "$LOG/rdc"
    export ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RENDERDOC_CAPFILE="$LOG/rdc/fsr4" RENDERDOC_HOOK_EGL=0
fi
# Proton log in the session folder (backtraces of a crash)
export PROTON_LOG=1 PROTON_LOG_DIR="$LOG"
# FS4_CAPTURE=1: record every pipeline the game creates (Steam's Fossilize layer) into $LOG/pipelines*.foz,
# to replay them offline against another driver build (fossilize_replay)
if [[ "${FS4_CAPTURE:-0}" == 1 ]]; then
    export ENABLE_VK_LAYER_VALVE_steam_fossilize_1=1 STEAM_FOSSILIZE_DUMP_PATH="$LOG/pipelines"
fi
# vkd3d-proton: expose D3D12 WMMA on ANV (wave32, u8 accumulator), and the RDNA3-style accumulator layout fixup
# wmma_rdna3_workaround (not just the layout part, wmma_rdna3_layout): wineland's amdxc64 shim only reports the
# emulated FP8 wave matrices (AGS extension 100, vkd3d says no to native FP8, 101) when DXIL_SPIRV_CONFIG is exactly
# this, and FSR4 asks it before anything else. Besides the accumulator layout fixup it sets FP16 RTZ rounding.
[[ "${FS4_OFF:-0}" != 1 ]] && export VKD3D_XMX_WMMA="${FS4_WMMA:-1}" DXIL_SPIRV_CONFIG=wmma_rdna3_workaround FS4SHIM_LOG="$(w "$LOG/fs4shim.log")" \
    FS4SHIM_MODEL="$FS4_MODEL"
export VKD3D_DEBUG="${VKD3D_DEBUG:-warn}" VKD3D_LOG_FILE="$(w "$LOG/vkd3d.log")"
if [[ "${FS4_DIAG:-0}" == 1 ]]; then
    mkdir -p "$LOG/shaders"
    export VKD3D_SHADER_DUMP_PATH="$(w "$LOG/shaders")"
    # vkd3d's disk cache would hand back shaders compiled in earlier sessions without compiling (or dumping) them
    export VKD3D_SHADER_CACHE_PATH=0
fi
if [[ "${FS4_OFF:-0}" != 1 ]]; then
    ovr="amdxcffx64=d"
    [[ "$MODE" == optiscaler ]] && ovr="$ovr;dxgi=n,b;amd_fidelityfx_upscaler_dx12=n;amd_fidelityfx_upscaler_dx12_fsr4=n"
    [[ "$MODE" == optiscaler && "${INPUTS:-}" == dlss ]] && ovr="$ovr;nvapi64=n"
    [[ "$MODE" == native ]] && ovr="$ovr;amd_fidelityfx_dx12=n;amd_fidelityfx_upscaler_dx12=n;amd_fidelityfx_upscaler_dx12_fsr4=n"
    export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}$ovr"
fi
unset PROTON_FSR4_UPGRADE

{
    echo "game_dir=$GAME_DIR"
    echo "fsr4=$(md5sum "$FSR4_DLL" | cut -d' ' -f1) mode=$MODE inputs=${INPUTS:--} model=$FS4_MODEL int8=$FS4_INT8 promote_f16=$INTEL_PROMOTE_F16 wmma=${FS4_WMMA:-1} off=${FS4_OFF:-0}"
    echo "date=$(date -Is)"
} > "$LOG/session.txt"
echo "fsr4-launch: staged in $GAME_DIR; logs in $LOG" >&2

"$@"
status=$?
grep -iE "WMMA|XMX_WMMA|amdxc|cooperative" "$LOG/vkd3d.log" 2>/dev/null | sort | uniq -c | head -20 >> "$LOG/session.txt"
exit $status
