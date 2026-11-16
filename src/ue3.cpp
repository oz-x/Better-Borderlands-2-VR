#include "ue3.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace ue3 {

namespace {

struct FNameEntry {
    uint8_t pad0[0x08];
    int32_t flags;
    void* pad1;
    union {
        char ansi[1024];
        wchar_t wide[1024];
    } name;
};

TArray<UObject*>* g_objects = nullptr;
TArray<FNameEntry*>* g_names = nullptr;
uintptr_t g_base = 0;
uintptr_t g_size = 0;

using ProcessEventFn = void(__fastcall*)(UObject*, void*, UFunction*, void*, void*);
ProcessEventFn g_process_event_original = nullptr;
ProcessEventObserver g_observer = nullptr;

std::mutex g_name_cache_mutex;
std::unordered_map<std::string, int32_t> g_name_cache;

// ---- signature scanning ----
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match
};

Pattern parse_pattern(const char* text) {
    Pattern p;
    const char* c = text;
    while (*c != '\0') {
        if (*c == ' ') {
            ++c;
            continue;
        }
        if (c[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            c += (c[1] == '?') ? 2 : 1;
            continue;
        }
        char hex[3] = {c[0], c[1], 0};
        p.bytes.push_back(static_cast<uint8_t>(strtoul(hex, nullptr, 16)));
        p.mask.push_back(1);
        c += 2;
    }
    return p;
}

}  // namespace

uintptr_t find_pattern(const char* text) {
    const Pattern p = parse_pattern(text);
    const auto* start = reinterpret_cast<const uint8_t*>(g_base);
    const size_t n = p.bytes.size();
    for (size_t i = 0; i + n <= g_size; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            if (p.mask[j] != 0 && start[i + j] != p.bytes[j]) {
                break;
            }
        }
        if (j == n) {
            return g_base + i;
        }
    }
    return 0;
}

namespace {

// Patterns from bl-sdk/unrealsdk (src/unrealsdk/game/bl2).
constexpr const char* GOBJECTS_SIG = "8B 0D ?? ?? ?? ?? 8B 04 ?? 8B 40 ?? 25 00 02 00 00";
constexpr const char* GNAMES_SIG = "A3 ?? ?? ?? ?? 8B 45 ?? 89 03";
constexpr const char* PROCESS_EVENT_SIG =
    "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 ?? ?? ?? ?? 50 83 EC 50 A1 ?? ?? ?? ?? 33 C5 89 45 ?? "
    "53 56 57 50 8D 45 ?? 64 A3 ?? ?? ?? ?? 8B F1";

void __fastcall process_event_hook(UObject* obj, void* edx, UFunction* func, void* params,
                                   void* result) {
    ProcessEventObserver observer = g_observer;
    if (observer != nullptr) {
        observer(obj, func, params, false);
    }
    g_process_event_original(obj, edx, func, params, result);
    if (observer != nullptr) {
        observer(obj, func, params, true);
    }
}

bool name_equals(FName name, const char* text) {
    if (g_names == nullptr || name.index < 0 || name.index >= g_names->count) {
        return false;
    }
    const FNameEntry* entry = g_names->data[name.index];
    if (entry == nullptr || (entry->flags & 1) != 0) {
        return false;
    }
    return _stricmp(entry->name.ansi, text) == 0;
}

}  // namespace

bool init() {
    if (g_objects != nullptr) {
        return true;
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    g_base = reinterpret_cast<uintptr_t>(exe);
    g_size = nt->OptionalHeader.SizeOfImage;

    const uintptr_t gobjects_match = find_pattern(GOBJECTS_SIG);
    const uintptr_t gnames_match = find_pattern(GNAMES_SIG);
    const uintptr_t process_event = find_pattern(PROCESS_EVENT_SIG);
    LOG("sigscan: GObjects@%p GNames@%p ProcessEvent@%p", (void*)gobjects_match,
        (void*)gnames_match, (void*)process_event);
    if (gobjects_match == 0 || gnames_match == 0 || process_event == 0) {
        LOG("ERROR: engine signatures not found, VR disabled");
        return false;
    }
    g_objects = *reinterpret_cast<TArray<UObject*>**>(gobjects_match + 2);
    g_names = *reinterpret_cast<TArray<FNameEntry*>**>(gnames_match + 1);

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("ERROR: MinHook init failed %d", status);
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(process_event),
                      reinterpret_cast<void*>(&process_event_hook),
                      reinterpret_cast<void**>(&g_process_event_original)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(process_event)) != MH_OK) {
        LOG("ERROR: failed to hook ProcessEvent");
        return false;
    }
    LOG("UE3 ready: GObjects=%p GNames=%p", (void*)g_objects, (void*)g_names);
    return true;
}

bool ready() {
    return g_objects != nullptr && g_objects->count > 0 && g_names != nullptr &&
           g_names->count > 0;
}

uintptr_t module_base() {
    return g_base;
}
uintptr_t module_size() {
    return g_size;
}

std::string name_string(FName name) {
    if (g_names == nullptr || name.index < 0 || name.index >= g_names->count) {
        return {};
    }
    const FNameEntry* entry = g_names->data[name.index];
    if (entry == nullptr) {
        return {};
    }
    std::string out;
    if ((entry->flags & 1) != 0) {
        for (const wchar_t* w = entry->name.wide; *w != 0; ++w) {
            out.push_back(static_cast<char>(*w < 128 ? *w : '?'));
        }
    } else {
        out = entry->name.ansi;
    }
    if (name.number > 0) {
        out += "_" + std::to_string(name.number - 1);
    }
    return out;
}

std::string object_name(const UObject* obj) {
    return obj == nullptr ? std::string("None") : name_string(name_of(obj));
}

std::string full_name(const UObject* obj) {
    if (obj == nullptr) {
        return "None";
    }
    std::string path = object_name(obj);
    for (const UObject* o = outer_of(obj); o != nullptr; o = outer_of(o)) {
        path = object_name(o) + "." + path;
    }
    return object_name(class_of(obj)) + " " + path;
}

int32_t find_name_index(const char* name) {
    {
        std::lock_guard lock(g_name_cache_mutex);
        auto it = g_name_cache.find(name);
        if (it != g_name_cache.end()) {
            return it->second;
        }
    }
    if (g_names == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < g_names->count; ++i) {
        const FNameEntry* entry = g_names->data[i];
        if (entry != nullptr && (entry->flags & 1) == 0 && _stricmp(entry->name.ansi, name) == 0) {
            std::lock_guard lock(g_name_cache_mutex);
            g_name_cache[name] = i;
            return i;
        }
    }
    return -1;
}

int32_t object_count() {
    return g_objects == nullptr ? 0 : g_objects->count;
}

UObject* object_at(int32_t index) {
    if (g_objects == nullptr || index < 0 || index >= g_objects->count) {
        return nullptr;
    }
    return g_objects->data[index];
}

// Reads an object's GObjects index; -1 if the memory is no longer readable (the object was
// freed and its pages released). No C++ objects here: __try needs a function without unwinding.
int32_t read_internal_index(const UObject* obj) {
    __try {
        return field<int32_t>(obj, off::object_internal_index);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

bool is_live(const UObject* obj) {
    if (obj == nullptr || g_objects == nullptr) {
        return false;
    }
    // The pointer may be stale (even freed): only trust it if GObjects still maps its slot
    // back to it.
    const int32_t index = read_internal_index(obj);
    return index >= 0 && index < g_objects->count && g_objects->data[index] == obj;
}

bool is_default_object(const UObject* obj) {
    return (field<uint32_t>(obj, 0x08) & 0x200) != 0 ||
           object_name(obj).rfind("Default__", 0) == 0;
}

bool is_a(const UObject* obj, const char* class_name) {
    for (const UObject* c = class_of(obj); c != nullptr; c = field<UObject*>(c, off::struct_super)) {
        if (name_equals(name_of(c), class_name)) {
            return true;
        }
    }
    return false;
}

bool is_a(const UObject* obj, const UClass* cls) {
    for (const UObject* c = class_of(obj); c != nullptr; c = field<UObject*>(c, off::struct_super)) {
        if (c == cls) {
            return true;
        }
    }
    return false;
}

UClass* find_class(const char* name) {
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = g_objects->data[i];
        if (obj != nullptr && name_equals(name_of(obj), name) &&
            name_equals(name_of(class_of(obj)), "Class")) {
            return obj;
        }
    }
    return nullptr;
}

UObject* find_object(const char* class_name, const char* path_name) {
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = g_objects->data[i];
        if (obj == nullptr || !name_equals(name_of(class_of(obj)), class_name)) {
            continue;
        }
        std::string full = full_name(obj);
        const size_t space = full.find(' ');
        if (_stricmp(full.c_str() + space + 1, path_name) == 0) {
            return obj;
        }
    }
    return nullptr;
}

UObject* find_first_instance(const char* class_name) {
    for (int32_t i = 0; i < object_count(); ++i) {
        UObject* obj = g_objects->data[i];
        if (obj != nullptr && is_a(obj, class_name) && !is_default_object(obj)) {
            return obj;
        }
    }
    return nullptr;
}

UProperty* find_property(const UStruct* type, const char* name) {
    for (const UStruct* s = type; s != nullptr; s = field<UStruct*>(s, off::struct_super)) {
        for (UObject* f = field<UObject*>(s, off::struct_children); f != nullptr;
             f = field<UObject*>(f, off::field_next)) {
            if (name_equals(name_of(f), name)) {
                return f;
            }
        }
    }
    return nullptr;
}

int32_t property_offset(const UStruct* type, const char* name) {
    UProperty* prop = find_property(type, name);
    return prop == nullptr ? -1 : field<int32_t>(prop, off::property_offset);
}

BoolRef find_bool(const UStruct* type, const char* name) {
    BoolRef out;
    UProperty* prop = find_property(type, name);
    if (prop != nullptr && name_equals(name_of(class_of(prop)), "BoolProperty")) {
        out.offset = field<int32_t>(prop, off::property_offset);
        out.mask = field<uint32_t>(prop, off::bool_property_mask);
    }
    return out;
}

UFunction* find_function(const UObject* obj, const char* name) {
    UProperty* f = find_property(class_of(obj), name);
    if (f != nullptr && name_equals(name_of(class_of(f)), "Function")) {
        return f;
    }
    return nullptr;
}

void call_function(UObject* obj, UFunction* func, void* params) {
    g_process_event_original(obj, nullptr, func, params, nullptr);
}

void console_command(UObject* player_controller, const wchar_t* command) {
    UFunction* func = find_function(player_controller, "ConsoleCommand");
    if (func == nullptr) {
        LOG("console_command: ConsoleCommand not found");
        return;
    }
    const uint16_t size = field<uint16_t>(func, off::function_params_size);
    std::vector<uint8_t> params(size + 16, 0);
    const int32_t cmd_off = property_offset(func, "Command");
    const int32_t log_off = property_offset(func, "bWriteToLog");
    std::wstring copy = command;
    auto& str = field<FString>(params.data(), cmd_off < 0 ? 0 : cmd_off);
    str.data = copy.data();
    str.count = static_cast<int32_t>(copy.size() + 1);
    str.max = str.count;
    if (log_off >= 0) {
        field<uint32_t>(params.data(), log_off) = 1;
    }
    LOG("console: %ls", command);
    call_function(player_controller, func, params.data());
    // The returned FString (if any) is engine-owned and intentionally leaked (tiny, rare).
}

void set_process_event_observer(ProcessEventObserver observer) {
    g_observer = observer;
}

}  // namespace ue3
