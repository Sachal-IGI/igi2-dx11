/* What the renderer needs from the DLL it is built into (igi2.exe 1.3 plugin). */
#ifndef IGI2_DX11_HOST_H
#define IGI2_DX11_HOST_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <windows.h>

/* log line: module name, text */
void Unified_Log(const char *module, const char *message);
/* PostFX in the host: frame textures are released before Reset, the frame is graded at Present */
void Unified_PostFxDeviceLost(void);
void Unified_PostFxPresentTest(void *device);
#endif
