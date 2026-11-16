#include "xr.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>
#include <vector>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace bl2vr::xr {

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
bool g_running = false;
std::atomic<bool> g_frame_open{false};          // xrBeginFrame called, xrEndFrame owed
std::atomic<bool> g_recenter_requested{false};  // runtime reported a LOCAL space change
LUID g_luid{};
bool g_have_luid = false;
XrViewConfigurationView g_view_config[2]{};
uint64_t g_frame_counter = 0;

// Serialises session begin/end (game thread) against frame submission (render thread).
std::mutex g_session_mutex;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t width = 0;
    uint32_t height = 0;
};
enum { kLeft, kRight, kScreen, kSwapchainCount };
Swapchain g_swapchains[kSwapchainCount];
int64_t g_swapchain_format = 0;

constexpr int kFramesInFlight = 3;
ComPtr<ID3D12CommandAllocator> g_allocators[kFramesInFlight];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
uint64_t g_fence_values[kFramesInFlight]{};
uint64_t g_fence_next = 1;
HANDLE g_fence_event = nullptr;
int g_allocator_index = 0;

bool check(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        char text[XR_MAX_RESULT_STRING_SIZE] = "?";
        if (g_instance != XR_NULL_HANDLE) {
            xrResultToString(g_instance, result, text);
        }
        LOG("OpenXR %s failed: %s (%d)", what, text, result);
        return false;
    }
    return true;
}

bool pick_format() {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());
    // The game writes sRGB-encoded BGRA8; copying the bytes into an *_SRGB image keeps them
    // correct. A plain UNORM image is only a fallback (colours will look washed out).
    for (int64_t wanted : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), wanted) != formats.end()) {
            g_swapchain_format = wanted;
            LOG("Swapchain format %lld", g_swapchain_format);
            return true;
        }
    }
    LOG("OpenXR runtime offers no BGRA8 swapchain format");
    return false;
}

bool ensure_swapchain(Swapchain& sc, uint32_t width, uint32_t height) {
    if (sc.handle != XR_NULL_HANDLE && sc.width == width && sc.height == height) {
        return true;
    }
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
        sc = Swapchain{};
    }
    if (g_swapchain_format == 0 && !pick_format()) {
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = g_swapchain_format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(xrCreateSwapchain(g_session, &info, &sc.handle), "xrCreateSwapchain")) {
        sc.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(sc.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
    sc.width = width;
    sc.height = height;
    LOG("Created %ux%u swapchain (%u images)", width, height, count);
    return true;
}

bool create_copy_resources() {
    for (auto& allocator : g_allocators) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&allocator)))) {
            return false;
        }
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_list)))) {
        return false;
    }
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void begin_copies() {
    const int index = g_allocator_index;
    if (g_fence->GetCompletedValue() < g_fence_values[index]) {
        g_fence->SetEventOnCompletion(g_fence_values[index], g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
    g_allocators[index]->Reset();
    g_list->Reset(g_allocators[index].Get(), nullptr);
}

uint64_t submit_copies() {
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    const int index = g_allocator_index;
    g_allocator_index = (g_allocator_index + 1) % kFramesInFlight;
    g_fence_values[index] = g_fence_next++;
    g_queue->Signal(g_fence.Get(), g_fence_values[index]);
    return g_fence_values[index];
}

// Acquires an image of `sc`, records the copy from `source` and returns false on failure.
bool copy_into(Swapchain& sc, ID3D12Resource* source, uint32_t width, uint32_t height) {
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(xrAcquireSwapchainImage(sc.handle, &acquire, &image), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100000000;  // 100 ms
    if (!check(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &release);
        return false;
    }
    ID3D12Resource* target = sc.images[image].texture;
    // Runtimes hand colour images out in RENDER_TARGET state and expect them back in it.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_list->ResourceBarrier(1, &barrier);
    return true;
}

void release_image(Swapchain& sc) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(xrReleaseSwapchainImage(sc.handle, &release), "xrReleaseSwapchainImage");
}

void submit_end(const FrameInfo& frame, const XrCompositionLayerBaseHeader* layer) {
    // A frame the game thread had to drop was already discarded by the next xrBeginFrame.
    if (!g_frame_open.exchange(false)) {
        return;
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.display_time;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = layer != nullptr ? 1 : 0;
    end.layers = layer != nullptr ? &layer : nullptr;
    check(xrEndFrame(g_session, &end), "xrEndFrame");
}

}  // namespace

bool init_instance() {
    if (g_instance != XR_NULL_HANDLE) {
        return true;
    }
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "Borderlands 2 VR");
    info.applicationInfo.applicationVersion = 1;
    strcpy_s(info.applicationInfo.engineName, "Unreal Engine 3");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    if (!check(xrCreateInstance(&info, &g_instance), "xrCreateInstance")) {
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &props);
    LOG("OpenXR runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
        XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(xrGetSystem(g_instance, &system_info, &g_system), "xrGetSystem")) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &system_props);
    LOG("Headset: %s", system_props.systemName);

    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (get_requirements != nullptr &&
        check(get_requirements(g_instance, g_system, &requirements), "D3D12 requirements")) {
        g_luid = requirements.adapterLuid;
        g_have_luid = true;
    }

    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (count != 2) {
        LOG("Unexpected view count %u", count);
        return false;
    }
    g_view_config[0].type = g_view_config[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(g_instance, g_system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
                                      g_view_config);
    LOG("Recommended eye size %ux%u", g_view_config[0].recommendedImageRectWidth,
        g_view_config[0].recommendedImageRectHeight);
    return true;
}

bool available() {
    return g_instance != XR_NULL_HANDLE;
}

bool adapter_luid(LUID* out) {
    if (!g_have_luid) {
        return false;
    }
    *out = g_luid;
    return true;
}

bool create_session(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g_instance == XR_NULL_HANDLE) {
        return false;
    }
    if (g_session != XR_NULL_HANDLE) {
        return true;
    }
    g_device = device;
    g_queue = queue;
    if (!create_copy_resources()) {
        LOG("Failed to create D3D12 copy resources");
        return false;
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g_system;
    if (!check(xrCreateSession(g_instance, &info, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (!check(xrCreateReferenceSpace(g_session, &space, &g_local), "xrCreateReferenceSpace")) {
        return false;
    }
    LOG("OpenXR session created");
    input_init(g_instance, g_session, g_local);
    return true;
}

bool session_running() {
    return g_running;
}

bool consume_recenter_request() {
    return g_recenter_requested.exchange(false);
}

uint32_t recommended_width() {
    return g_view_config[0].recommendedImageRectWidth;
}
uint32_t recommended_height() {
    return g_view_config[0].recommendedImageRectHeight;
}

void poll_events() {
    if (g_instance == XR_NULL_HANDLE) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            LOG("OpenXR session state %d", changed.state);
            std::lock_guard lock(g_session_mutex);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = check(xrBeginSession(g_session, &begin), "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                check(xrEndSession(g_session), "xrEndSession");
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOG("OpenXR instance loss pending");
            g_running = false;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The user recentred from the headset (e.g. holding the Meta button).
            g_recenter_requested = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool begin_frame(FrameInfo& out) {
    if (!g_running) {
        return false;
    }
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!check(xrWaitFrame(g_session, nullptr, &state), "xrWaitFrame")) {
        return false;
    }
    if (!check(xrBeginFrame(g_session, nullptr), "xrBeginFrame")) {
        return false;
    }
    g_frame_open = true;
    out = FrameInfo{};
    out.id = ++g_frame_counter;
    out.display_time = state.predictedDisplayTime;
    out.should_render = state.shouldRender == XR_TRUE;

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = g_local;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    out.views[0].type = out.views[1].type = XR_TYPE_VIEW;
    if (check(xrLocateViews(g_session, &locate, &view_state, 2, &count, out.views),
              "xrLocateViews") &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        out.views_valid = true;
    }
    return true;
}

uint64_t end_frame_stereo(const FrameInfo& frame, ID3D12Resource* left, ID3D12Resource* right,
                          uint32_t width, uint32_t height, float tan_half_x, float tan_half_y) {
    std::lock_guard lock(g_session_mutex);
    if (!g_running) {
        return 0;
    }
    if (!frame.should_render || !frame.views_valid ||
        !ensure_swapchain(g_swapchains[kLeft], width, height) ||
        !ensure_swapchain(g_swapchains[kRight], width, height)) {
        submit_end(frame, nullptr);
        return 0;
    }
    begin_copies();
    XrCompositionLayerProjectionView views[2]{};
    ID3D12Resource* sources[2] = {left, right};
    bool ok = true;
    for (int eye = 0; eye < 2; ++eye) {
        Swapchain& sc = g_swapchains[eye];
        ok = copy_into(sc, sources[eye], width, height) && ok;
        views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[eye].pose = frame.views[eye].pose;
        // The game rendered a symmetric frustum centred on the eye's forward axis.
        views[eye].fov.angleLeft = -std::atan(tan_half_x);
        views[eye].fov.angleRight = std::atan(tan_half_x);
        views[eye].fov.angleUp = std::atan(tan_half_y);
        views[eye].fov.angleDown = -std::atan(tan_half_y);
        views[eye].subImage.swapchain = sc.handle;
        views[eye].subImage.imageRect.extent = {static_cast<int32_t>(width),
                                                static_cast<int32_t>(height)};
    }
    const uint64_t fence_value = submit_copies();
    for (int eye = 0; eye < 2; ++eye) {
        release_image(g_swapchains[eye]);
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_local;
    layer.viewCount = 2;
    layer.views = views;
    submit_end(frame, ok ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) : nullptr);
    return fence_value;
}

uint64_t end_frame_screen(const FrameInfo& frame, ID3D12Resource* image, uint32_t width,
                          uint32_t height, const XrPosef& pose, float width_metres) {
    std::lock_guard lock(g_session_mutex);
    if (!g_running) {
        return 0;
    }
    Swapchain& sc = g_swapchains[kScreen];
    if (!frame.should_render || !ensure_swapchain(sc, width, height)) {
        submit_end(frame, nullptr);
        return 0;
    }
    begin_copies();
    const bool ok = copy_into(sc, image, width, height);
    const uint64_t fence_value = submit_copies();
    release_image(sc);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = g_local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = sc.handle;
    quad.subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
    quad.pose = pose;
    quad.size = {width_metres, width_metres * height / static_cast<float>(width)};
    submit_end(frame, ok ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad) : nullptr);
    return fence_value;
}

void end_frame_empty(const FrameInfo& frame) {
    std::lock_guard lock(g_session_mutex);
    if (g_running) {
        submit_end(frame, nullptr);
    }
}

ID3D12Fence* copy_fence() {
    return g_fence.Get();
}

}  // namespace bl2vr::xr
