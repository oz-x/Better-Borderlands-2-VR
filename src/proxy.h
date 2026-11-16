#pragma once
#include <windows.h>

namespace bl2vr {

HMODULE system_d3d9();
FARPROC system_proc(const char* name);
void proxy_resolve_exports();

}  // namespace bl2vr
