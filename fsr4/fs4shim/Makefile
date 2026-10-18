# fs4shim: proxy amd_fidelityfx_upscaler_dx12.dll (see fs4shim.c and docs/fsr4.md). Needs mingw-w64.
CC = x86_64-w64-mingw32-gcc
amd_fidelityfx_upscaler_dx12.dll: fs4shim.c
	$(CC) -O2 -s -shared -o $@ $< -Wl,--kill-at
clean:
	rm -f amd_fidelityfx_upscaler_dx12.dll
