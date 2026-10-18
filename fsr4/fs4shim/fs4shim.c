/* fs4shim: amd_fidelityfx_upscaler_dx12.dll proxy that lets FSR4's FP8 provider run on GPUs that have the
 * cooperative matrices it needs but no AMD kernel driver behind them (fs4-xmx: Intel Arc through vkd3d-proton).
 *
 * The FidelityFX SDK upscaler DLL only lists FSR4 when ffxProvider_FSR4::IsSupported(device) says yes, and that
 * asks two things: the 16x16x16 wave matrices through amdxc64 (AmdExtD3DDevice8::GetWaveMatrixProperties and the
 * WMMA shader intrinsics), and a description of the GPU read from the AMD kernel driver's private adapter data
 * (D3DKMTQueryAdapterInfo), which only exists on Windows with an AMD driver. The context creation asks the first
 * one again. This proxy loads the real DLL (FS4SHIM_REAL, default amd_fidelityfx_upscaler_dx12_fsr4.dll beside
 * it), finds ffxProvider_FSR4's vtable through its RTTI and replaces IsSupported with the wave matrix check alone,
 * calling the DLL's own check. If the code is not the one this was written against (FSR4 4.1.1), it patches
 * nothing and the DLL behaves as shipped. FS4SHIM_LOG=<file> logs what it did.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t (*PFN_ffx)(void *, void *);
typedef uint32_t (*PFN_ffx3)(void *, void *, void *);
static HMODULE real;
static PFN_ffx3 p_create;
static PFN_ffx p_destroy, p_configure, p_query, p_dispatch;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static uint8_t (*wave_matrix_check)(void *device);

static void
logf_(const char *fmt, ...)
{
   char path[MAX_PATH];
   if (!GetEnvironmentVariableA("FS4SHIM_LOG", path, sizeof(path)))
      return;
   FILE *f = fopen(path, "a");
   if (!f)
      return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

/* ffxProvider_FSR4::IsSupported(this, device) without the AMD kernel driver query */
static uint8_t
fsr4_is_supported(void *self, void *device)
{
   (void)self;
   /* Called a lot (games poll the upscaler versions, often with no device, ~1000 calls every 2 s in Cyberpunk):
    * a file write per call stalled the frame. Log changes, at most 16 lines. */
   static void *last_device = (void *)-1;
   static uint8_t last_ok = 2;
   uint8_t ok = device && wave_matrix_check(device);
   static LONG logged;
   if ((device != last_device || ok != last_ok) && InterlockedIncrement(&logged) <= 16) {
      last_device = device;
      last_ok = ok;
      logf_("fs4shim: FSR4 IsSupported(%p) -> %u (changes only, first 16)", device, ok);
   }
   return ok;
}

/* ffxProvider_FSR4_Int8::IsSupported(this, device): FSR4's INT8 model (8-bit dot products, no wave matrices)
 * asks only the AMD kernel driver's GPU description (RDNA3 family, or one device id). */
static uint8_t
fsr4_int8_is_supported(void *self, void *device)
{
   (void)self;
   return device != NULL;
}

static uint8_t
fsr4_unsupported(void *self, void *device)
{
   (void)self; (void)device;
   return 0;
}

struct sections { uint8_t *base, *text, *rdata, *data; size_t text_sz, rdata_sz, data_sz; };

/* RTTI: TypeDescriptor name -> CompleteObjectLocator -> vtable */
static void **
find_vtable(const struct sections *m, const char *name)
{
   const size_t len = strlen(name) + 1;
   uint32_t td_rva = 0;
   for (uint8_t *p = m->data; p + len < m->data + m->data_sz; p++)
      if (!memcmp(p, name, len)) { td_rva = (uint32_t)(p - 16 - m->base); break; }
   if (!td_rva) return NULL;

   uint32_t col_rva = 0;
   for (uint8_t *p = m->rdata; p + 24 <= m->rdata + m->rdata_sz; p += 4) {
      const uint32_t *c = (const uint32_t *)p;
      if (c[0] == 1 && c[3] == td_rva && c[5] == (uint32_t)(p - m->base)) { col_rva = c[5]; break; }
   }
   if (!col_rva) return NULL;

   const uint64_t col_va = (uint64_t)m->base + col_rva;
   for (uint8_t *p = m->rdata; p + 16 <= m->rdata + m->rdata_sz; p += 8)
      if (*(uint64_t *)p == col_va) return (void **)(p + 8);
   return NULL;
}

static int
set_vtable_entry(void **slot, void *fn)
{
   DWORD old;
   if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
   *slot = fn;
   VirtualProtect(slot, sizeof(void *), old, &old);
   return 1;
}

/* FS4SHIM_MODEL: fp8 (default; the FP8 model on the wave matrices) or int8 (the INT8 model, 8-bit dot products) */
static void
patch_fsr4_provider(HMODULE mod)
{
   struct sections m = { .base = (uint8_t *)mod };
   IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(m.base + ((IMAGE_DOS_HEADER *)m.base)->e_lfanew);
   IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
   for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
      uint8_t *va = m.base + sec[i].VirtualAddress;
      if (!memcmp(sec[i].Name, ".text", 5)) { m.text = va; m.text_sz = sec[i].Misc.VirtualSize; }
      if (!memcmp(sec[i].Name, ".rdata", 6)) { m.rdata = va; m.rdata_sz = sec[i].Misc.VirtualSize; }
      if (!memcmp(sec[i].Name, ".data", 5) && sec[i].Name[5] == 0) { m.data = va; m.data_sz = sec[i].Misc.VirtualSize; }
   }
   if (!m.text || !m.rdata || !m.data) { logf_("fs4shim: sections not found"); return; }

   char model[16] = "fp8";
   GetEnvironmentVariableA("FS4SHIM_MODEL", model, sizeof(model));
   const int int8 = !strcmp(model, "int8");

   void **fp8_vt = find_vtable(&m, ".?AVffxProvider_FSR4@@");
   void **int8_vt = find_vtable(&m, ".?AVffxProvider_FSR4_Int8@@");
   if (!fp8_vt || !int8_vt) { logf_("fs4shim: FSR4 provider vtables not found, not patching"); return; }

   /* FSR4 4.1.1 FP8 IsSupported: push rbx; sub rsp,0x40; mov rbx,rdx; test rdx,rdx; je; mov rcx,rdx; call <wave matrix check> */
   static const uint8_t fp8_head[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xda, 0x48, 0x85, 0xd2, 0x74 };
   /* INT8 IsSupported: sub rsp,0x48; mov rax,rdx; test rdx,rdx; je */
   static const uint8_t int8_head[] = { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0xc2, 0x48, 0x85, 0xd2, 0x74 };
   uint8_t *fp8_fn = fp8_vt[2], *int8_fn = int8_vt[2];
   if (fp8_fn < m.text || fp8_fn >= m.text + m.text_sz || memcmp(fp8_fn, fp8_head, sizeof(fp8_head)) ||
       memcmp(fp8_fn + 14, (const uint8_t[]){ 0x48, 0x8b, 0xca, 0xe8 }, 4) ||
       int8_fn < m.text || int8_fn >= m.text + m.text_sz || memcmp(int8_fn, int8_head, sizeof(int8_head))) {
      logf_("fs4shim: unexpected FSR4 IsSupported code (%p, %p), not patching", fp8_fn, int8_fn);
      return;
   }
   wave_matrix_check = (void *)(fp8_fn + 22 + *(int32_t *)(fp8_fn + 18));
   if ((uint8_t *)wave_matrix_check < m.text || (uint8_t *)wave_matrix_check >= m.text + m.text_sz) {
      logf_("fs4shim: wave matrix check out of .text, not patching");
      return;
   }

   if (int8) {
      if (set_vtable_entry(&fp8_vt[2], (void *)fsr4_unsupported) &&
          set_vtable_entry(&int8_vt[2], (void *)fsr4_int8_is_supported))
         logf_("fs4shim: model int8: FSR4 INT8 offered on any device, FP8 off");
   } else if (set_vtable_entry(&fp8_vt[2], (void *)fsr4_is_supported)) {
      logf_("fs4shim: model fp8: patched ffxProvider_FSR4::IsSupported (vtable %p, wave matrix check %p)",
            fp8_vt, wave_matrix_check);
   }
}

static BOOL CALLBACK
init(INIT_ONCE *o, void *p, void **c)
{
   (void)o; (void)p; (void)c;
   char path[MAX_PATH], dir[MAX_PATH];
   if (!GetEnvironmentVariableA("FS4SHIM_REAL", path, sizeof(path))) {
      HMODULE self;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)init, &self);
      GetModuleFileNameA(self, dir, sizeof(dir));
      char *slash = strrchr(dir, '\\');
      if (slash) slash[1] = 0;
      snprintf(path, sizeof(path), "%samd_fidelityfx_upscaler_dx12_fsr4.dll", dir);
   }
   real = LoadLibraryA(path);
   if (!real) { logf_("fs4shim: cannot load %s (%lu)", path, GetLastError()); return TRUE; }
   logf_("fs4shim: loaded %s", path);
   p_create = (PFN_ffx3)GetProcAddress(real, "ffxCreateContext");
   p_destroy = (PFN_ffx)GetProcAddress(real, "ffxDestroyContext");
   p_configure = (PFN_ffx)GetProcAddress(real, "ffxConfigure");
   p_query = (PFN_ffx)GetProcAddress(real, "ffxQuery");
   p_dispatch = (PFN_ffx)GetProcAddress(real, "ffxDispatch");
   patch_fsr4_provider(real);
   return TRUE;
}

/* Timing (FS4SHIM_LOG set): every create/destroy with its duration, and once per second the number and total
 * time of the other calls, to see what an FSR4 frame or a hitch spends in the upscaler. */
static LARGE_INTEGER qpf;
static double
now_ms(void)
{
   LARGE_INTEGER t;
   if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
   QueryPerformanceCounter(&t);
   return t.QuadPart * 1000.0 / qpf.QuadPart;
}
static int timing = -1;
static struct { LONG n; double ms, max; } st[3];   /* query, configure, dispatch */
static double window_start;
static SRWLOCK st_lock = SRWLOCK_INIT;

static void
account(int k, double t0)
{
   double dt = now_ms() - t0;
   AcquireSRWLockExclusive(&st_lock);
   st[k].n++; st[k].ms += dt; if (dt > st[k].max) st[k].max = dt;
   double t = now_ms();
   if (!window_start) window_start = t;
   if (t - window_start >= 1000.0) {
      logf_("fs4shim: t=%.0fms  query %ld/%.1fms(max %.1f)  configure %ld/%.1fms(max %.1f)  dispatch %ld/%.1fms(max %.1f)",
            t, st[0].n, st[0].ms, st[0].max, st[1].n, st[1].ms, st[1].max, st[2].n, st[2].ms, st[2].max);
      memset(st, 0, sizeof(st));
      window_start = t;
   }
   ReleaseSRWLockExclusive(&st_lock);
}

static int
timing_on(void)
{
   if (timing < 0) timing = GetEnvironmentVariableA("FS4SHIM_LOG", NULL, 0) > 0;
   return timing;
}

#define READY() (InitOnceExecuteOnce(&once, init, NULL, NULL), real)
__declspec(dllexport) uint32_t ffxCreateContext(void *ctx, void *desc, void *mem)
{
   if (!READY() || !p_create) return 1;
   double t0 = now_ms();
   uint32_t r = p_create(ctx, desc, mem);
   if (timing_on()) logf_("fs4shim: t=%.0fms ffxCreateContext type %#llx -> %u in %.1f ms", t0,
                          desc ? (unsigned long long)*(uint64_t *)desc : 0ull, r, now_ms() - t0);
   return r;
}
__declspec(dllexport) uint32_t ffxDestroyContext(void *ctx, void *mem)
{
   if (!READY() || !p_destroy) return 1;
   double t0 = now_ms();
   uint32_t r = p_destroy(ctx, mem);
   if (timing_on()) logf_("fs4shim: t=%.0fms ffxDestroyContext -> %u in %.1f ms", t0, r, now_ms() - t0);
   return r;
}
__declspec(dllexport) uint32_t ffxConfigure(void *ctx, void *desc)
{
   if (!READY() || !p_configure) return 1;
   double t0 = timing_on() ? now_ms() : 0;
   uint32_t r = p_configure(ctx, desc);
   if (t0) account(1, t0);
   return r;
}
__declspec(dllexport) uint32_t ffxQuery(void *ctx, void *desc)
{
   if (!READY() || !p_query) return 1;
   double t0 = timing_on() ? now_ms() : 0;
   uint32_t r = p_query(ctx, desc);
   if (t0) account(0, t0);
   return r;
}
__declspec(dllexport) uint32_t ffxDispatch(void *ctx, void *desc)
{
   if (!READY() || !p_dispatch) return 1;
   double t0 = timing_on() ? now_ms() : 0;
   uint32_t r = p_dispatch(ctx, desc);
   if (t0) account(2, t0);
   return r;
}
