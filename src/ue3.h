#pragma once
// Minimal Unreal Engine 3 (Borderlands 2, 32-bit) reflection layer.
// Object layout facts come from the open-source bl-sdk/unrealsdk project (willow2 flavour).
#include <cstdint>
#include <string>

namespace ue3 {

struct FName {
    int32_t index;
    int32_t number;
};

template <class T>
struct TArray {
    T* data;
    int32_t count;
    int32_t max;
};

using FString = TArray<wchar_t>;

struct FVector {
    float x, y, z;
};

struct FRotator {
    int32_t pitch, yaw, roll;
};

struct UObject;  // opaque; accessed through the helpers below
using UClass = UObject;
using UStruct = UObject;
using UFunction = UObject;
using UProperty = UObject;

// ---- object layout ----
namespace off {
constexpr uint32_t object_internal_index = 0x20;
constexpr uint32_t object_outer = 0x28;
constexpr uint32_t object_name = 0x2C;
constexpr uint32_t object_class = 0x34;
constexpr uint32_t field_next = 0x3C;
constexpr uint32_t property_array_dim = 0x40;
constexpr uint32_t property_element_size = 0x44;
constexpr uint32_t property_offset = 0x60;
constexpr uint32_t bool_property_mask = 0x80;
constexpr uint32_t struct_super = 0x48;
constexpr uint32_t struct_children = 0x4C;
constexpr uint32_t struct_property_size = 0x50;
constexpr uint32_t function_params_size = 0x9E;
}  // namespace off

template <class T>
inline T& field(const void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

inline UClass* class_of(const UObject* obj) {
    return field<UClass*>(obj, off::object_class);
}
inline UObject* outer_of(const UObject* obj) {
    return field<UObject*>(obj, off::object_outer);
}
inline FName name_of(const UObject* obj) {
    return field<FName>(obj, off::object_name);
}

// Resolves engine globals and installs the ProcessEvent detour. Must run after the executable
// has been unpacked (we call it from Direct3DCreate9).
bool init();
bool ready();
uintptr_t module_base();
uintptr_t module_size();
// First match of an IDA-style pattern ("8B 0D ?? ?? 25") inside the executable image.
uintptr_t find_pattern(const char* pattern);

// Name helpers.
std::string name_string(FName name);
std::string object_name(const UObject* obj);
std::string full_name(const UObject* obj);  // "ClassName Outer.Outer.Name"
// Returns -1 when the name does not exist (yet).
int32_t find_name_index(const char* name);

// Object lookup / liveness.
int32_t object_count();
UObject* object_at(int32_t index);
bool is_live(const UObject* obj);
UObject* find_object(const char* class_name, const char* path_name);
// First non-template object whose class (or a superclass) is called `class_name`.
UObject* find_first_instance(const char* class_name);
bool is_a(const UObject* obj, const char* class_name);
bool is_a(const UObject* obj, const UClass* cls);
// Class object by short name ("GFxMoviePlayer"), nullptr if not loaded.
UClass* find_class(const char* name);
bool is_default_object(const UObject* obj);

// Reflection.
UProperty* find_property(const UStruct* type, const char* name);
int32_t property_offset(const UStruct* type, const char* name);  // -1 if missing

// A resolved bool property (bitfield inside a dword).
struct BoolRef {
    int32_t offset = -1;
    uint32_t mask = 0;

    bool valid() const { return offset >= 0; }
    bool get(const UObject* obj) const {
        return valid() && (field<uint32_t>(obj, offset) & mask) != 0;
    }
    void set(UObject* obj, bool value) const {
        if (valid()) {
            uint32_t& bits = field<uint32_t>(obj, offset);
            bits = value ? (bits | mask) : (bits & ~mask);
        }
    }
};
BoolRef find_bool(const UStruct* type, const char* name);

// Calls a UFunction through ProcessEvent (bypassing our hook).
void call_function(UObject* obj, UFunction* func, void* params);
UFunction* find_function(const UObject* obj, const char* name);

// Runs a console command on a PlayerController.
void console_command(UObject* player_controller, const wchar_t* command);

// ProcessEvent observer. Called on the thread that runs ProcessEvent (the game thread).
// `post` is false before the original executes and true after it.
using ProcessEventObserver = void (*)(UObject* obj, UFunction* func, void* params, bool post);
void set_process_event_observer(ProcessEventObserver observer);

}  // namespace ue3
