// Debug helper: reports whether the active 32-bit OpenXR runtime has a headset available.
#include <windows.h>

#include <cstdio>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

int main() {
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "bl2vr probe");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&info, &instance);
    printf("xrCreateInstance: %d\n", r);
    if (XR_FAILED(r)) {
        return 1;
    }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &props);
    printf("runtime: %s\n", props.runtimeName);
    XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
    sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId id = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(instance, &sys, &id);
    printf("xrGetSystem: %d\n", r);
    if (XR_SUCCEEDED(r)) {
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        xrGetSystemProperties(instance, id, &sp);
        printf("system: %s\n", sp.systemName);
        XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t n = 0;
        xrEnumerateViewConfigurationViews(instance, id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          2, &n, views);
        printf("recommended %ux%u\n", views[0].recommendedImageRectWidth,
               views[0].recommendedImageRectHeight);
    }
    xrDestroyInstance(instance);
    return 0;
}
