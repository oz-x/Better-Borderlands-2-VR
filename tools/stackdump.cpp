// Debug helper: prints the call stacks of every thread of a 32-bit process.
// Usage: stackdump.exe <pid> [symbol search path]
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "dbghelp.lib")

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: stackdump <pid> [sympath]\n");
        return 1;
    }
    const DWORD pid = static_cast<DWORD>(atoi(argv[1]));
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (process == nullptr) {
        printf("OpenProcess failed %lu\n", GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, argc > 2 ? argv[2] : nullptr, TRUE);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        SuspendThread(thread);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(thread, &ctx);
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Esp;
        frame.AddrStack.Mode = AddrModeFlat;
        printf("=== thread %lu\n", te.th32ThreadID);
        for (int i = 0; i < 40; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, thread, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                break;
            }
            const DWORD64 pc = frame.AddrPC.Offset;
            char buffer[sizeof(SYMBOL_INFO) + 256]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULE64 module{sizeof(module)};
            SymGetModuleInfo64(process, pc, &module);
            if (SymFromAddr(process, pc, &displacement, symbol)) {
                printf("  %s!%s+0x%llx\n", module.ModuleName, symbol->Name, displacement);
            } else {
                printf("  %s+0x%llx\n", module.ModuleName, pc - module.BaseOfImage);
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return 0;
}
