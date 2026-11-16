#pragma once
#include <cstdarg>

namespace bl2vr {

void log_init(const wchar_t* path);
void logf(const char* fmt, ...);

}  // namespace bl2vr

#define LOG(...) ::bl2vr::logf(__VA_ARGS__)
