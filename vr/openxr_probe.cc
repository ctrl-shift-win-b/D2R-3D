// Asks the OpenXR runtime named by XR_RUNTIME_JSON which environment blend
// modes it advertises. Does not create a session and does not submit frames.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <openxr/openxr.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

template <class T>
T Load(HMODULE lib, const char* name) {
    return (T)GetProcAddress(lib, name);
}

const char* ResultName(XrResult r) {
    switch (r) {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    default: return "other";
    }
}

}  // namespace

int main() {
    wchar_t runtime[32768] = L"";
    GetEnvironmentVariableW(L"XR_RUNTIME_JSON", runtime, 32768);
    std::printf("XR_RUNTIME_JSON=%ls\n", runtime);

    HMODULE lib = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
    if (!lib) {
        std::printf("openxr_loader.dll failed to load (%lu)\n", GetLastError());
        return 1;
    }
    auto xrGetInstanceProcAddr = Load<PFN_xrGetInstanceProcAddr>(lib, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr) {
        std::printf("xrGetInstanceProcAddr missing\n");
        return 1;
    }

    PFN_xrCreateInstance xrCreateInstance = nullptr;
    PFN_xrDestroyInstance xrDestroyInstance = nullptr;
    PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
    PFN_xrGetSystem xrGetSystem = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes xrEnumerateEnvironmentBlendModes = nullptr;
    // Only the pre-instance functions may be queried with a null instance.
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance)) || !xrCreateInstance) {
        std::printf("xrCreateInstance missing\n");
        return 1;
    }

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "d2r-3d-probe");
    info.applicationInfo.applicationVersion = 1;
    strcpy_s(info.applicationInfo.engineName, "d2r-3d");
    info.applicationInfo.engineVersion = 1;
    const XrVersion versions[] = {XR_CURRENT_API_VERSION, XR_API_VERSION_1_0, XR_MAKE_VERSION(1, 0, 34), XR_MAKE_VERSION(1, 0, 0)};
    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = XR_ERROR_API_VERSION_UNSUPPORTED;
    for (XrVersion version : versions) {
        info.applicationInfo.apiVersion = version;
        result = xrCreateInstance(&info, &instance);
        std::printf("xrCreateInstance %u.%u.%u %s (%d)\n", XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version),
                    XR_VERSION_PATCH(version), ResultName(result), (int)result);
        if (XR_SUCCEEDED(result)) break;
    }
    if (XR_FAILED(result)) return 2;

    xrGetInstanceProcAddr(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&xrDestroyInstance);
    xrGetInstanceProcAddr(instance, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&xrGetInstanceProperties);
    xrGetInstanceProcAddr(instance, "xrGetSystem", (PFN_xrVoidFunction*)&xrGetSystem);
    xrGetInstanceProcAddr(instance, "xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction*)&xrEnumerateEnvironmentBlendModes);
    if (!xrDestroyInstance) {
        std::printf("xrDestroyInstance missing\n");
        return 1;
    }

    if (xrGetInstanceProperties) {
        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &props)))
            std::printf("runtime %s %u.%u.%u\n", props.runtimeName,
                        XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                        XR_VERSION_PATCH(props.runtimeVersion));
    }

    XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
    sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    result = xrGetSystem ? xrGetSystem(instance, &sys, &system) : XR_ERROR_FUNCTION_UNSUPPORTED;
    std::printf("xrGetSystem %s (%d) id=%llu\n", ResultName(result), (int)result, (unsigned long long)system);

    if (XR_SUCCEEDED(result) && xrEnumerateEnvironmentBlendModes) {
        uint32_t count = 0;
        xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
        std::vector<XrEnvironmentBlendMode> modes(count);
        if (count && XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()))) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* name = "unknown";
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) name = "OPAQUE";
                else if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE) name = "ADDITIVE";
                else if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) name = "ALPHA_BLEND";
                std::printf("blend %s (%d)\n", name, (int)modes[i]);
            }
        }
    }

    xrDestroyInstance(instance);
    return XR_SUCCEEDED(result) ? 0 : 3;
}
