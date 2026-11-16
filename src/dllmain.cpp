#include <windows.h>

#include "proxy.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        bl2vr::proxy_resolve_exports();
    }
    return TRUE;
}
