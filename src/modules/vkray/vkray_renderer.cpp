#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vkray_renderer.h"
#include "vkray_log_window.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <windows.h>

#define IDR_GLSLANG_EXE  200
#define IDR_SHADER_COMP  201
#define IDR_SHADER_VERT  202
#define IDR_SHADER_FRAG  203

namespace vkray {

// ============================================================================
// 辅助：定位 exe 目录
// ============================================================================
static std::string exeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* p = strrchr(path, '\\');
    if (p) *p = '\0';
    return std::string(path);
}

// ============================================================================
// 辅助：VkResult → 字符串
// ============================================================================
static const char* vr2s(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "<unknown>";
    }
}

// 一次性启用 Windows 控制台 ANSI 转义支持
static bool g_vkr_ansi_enabled = []() {
    return true;
}();

#define VKR_LOG(msg) do { \
    std::ostringstream _vkoss; \
    _vkoss << "vkray >> " << msg << "\n"; \
    VkrayLogWindow::appendText(_vkoss.str()); \
} while(0)
#define VKR_LOG_OK(msg) VKR_LOG(msg)
#define VKR_LOG_FAIL(msg) VKR_LOG("FAIL: " << msg)
#define VKR_LOG_STEP(msg) VKR_LOG(msg)

// ============================================================================
// 从资源提取文件到运行目录
// ============================================================================
static bool extractResource(int resourceId, const std::string& dstPath) {
    HRSRC hRes = FindResourceA(nullptr, MAKEINTRESOURCEA(resourceId), MAKEINTRESOURCEA(10));
    if (!hRes) {
        VKR_LOG_FAIL("FindResourceA failed for resource " << resourceId);
        return false;
    }
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) {
        VKR_LOG_FAIL("LoadResource failed for resource " << resourceId);
        return false;
    }
    void* pData = LockResource(hData);
    DWORD size = SizeofResource(nullptr, hRes);
    if (!pData || size == 0) {
        VKR_LOG_FAIL("LockResource/SizeofResource failed for resource " << resourceId);
        return false;
    }
    std::ofstream ofs(dstPath, std::ios::binary);
    if (!ofs) {
        VKR_LOG_FAIL("Cannot open " << dstPath << " for writing");
        return false;
    }
    ofs.write(static_cast<const char*>(pData), size);
    ofs.close();
    VKR_LOG("  Extracted resource " << resourceId << " -> " << dstPath << " (" << size << " bytes)");
    return true;
}

// ============================================================================
// 编译着色器：调用 glslang.exe
// ============================================================================
static bool compileShader(const std::string& glslangPath,
                          const std::string& srcPath,
                          const std::string& dstSpvPath) {
    std::string cmd = "\"" + glslangPath + "\" -V --target-env vulkan1.1 -o \"" + dstSpvPath + "\" \"" + srcPath + "\"";
    VKR_LOG("  Compiling: " << cmd);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hReadPipe, hWritePipe;
    CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES;
    BOOL ok = CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()),
                             nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);
    if (!ok) {
        VKR_LOG_FAIL("CreateProcess failed for glslang");
        CloseHandle(hReadPipe);
        return false;
    }
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        VkrayLogWindow::appendText(std::string("[glslang] ") + buf);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    if (exitCode != 0) {
        VKR_LOG_FAIL("glslang failed with exit code " << exitCode);
        return false;
    }
    VKR_LOG_OK("Shader compiled: " << dstSpvPath);
    return true;
}

// ============================================================================
// 准备着色器：提取资源 + 编译
// ============================================================================
static bool prepareShaders() {
    std::string dir = exeDir();
    std::string glslangPath = dir + "\\glslang.exe";
    std::string spvDir = dir + "\\spv\\";
    std::string shaderDir = dir + "\\shaders\\";

    CreateDirectoryA(spvDir.c_str(), nullptr);
    CreateDirectoryA(shaderDir.c_str(), nullptr);

    if (GetFileAttributesA(glslangPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        VKR_LOG("Extracting glslang.exe...");
        if (!extractResource(IDR_GLSLANG_EXE, glslangPath)) return false;
    }

    struct { int id; const char* name; } shaders[] = {
        { IDR_SHADER_COMP, "pathtracer.comp" },
        { IDR_SHADER_VERT, "present.vert" },
        { IDR_SHADER_FRAG, "present.frag" },
    };
    for (auto& s : shaders) {
        std::string srcPath = shaderDir + s.name;
        if (GetFileAttributesA(srcPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            VKR_LOG("Extracting " << s.name << "...");
            if (!extractResource(s.id, srcPath)) return false;
        }
    }

    struct { const char* src; const char* spv; } compileList[] = {
        { "pathtracer.comp", "pathtracer.comp.spv" },
        { "present.vert",    "present.vert.spv" },
        { "present.frag",    "present.frag.spv" },
    };
    for (auto& c : compileList) {
        std::string srcPath = shaderDir + c.src;
        std::string spvPath = spvDir + c.spv;
        if (!compileShader(glslangPath, srcPath, spvPath)) return false;
    }

    return true;
}

std::vector<uint32_t> ComputePathTracer::loadSpv(const std::string& path) {
    VKR_LOG("Loading SPIR-V: " << path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        VKR_LOG_FAIL("Failed to open SPIR-V: " << path);
        return {};
    }
    size_t size = static_cast<size_t>(f.tellg());
    VKR_LOG("  size=" << size << " bytes (" << (size/4) << " dwords)");
    if (size == 0 || (size % 4) != 0) {
        VKR_LOG_FAIL("Invalid SPIR-V size: " << path);
        return {};
    }
    f.seekg(0);
    std::vector<uint32_t> code(size / 4);
    f.read(reinterpret_cast<char*>(code.data()), size);
    VKR_LOG_OK("SPIR-V loaded: " << path);
    return code;
}

uint32_t ComputePathTracer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return ~0u;
}

ComputePathTracer::ComputePathTracer() {}
ComputePathTracer::~ComputePathTracer() { shutdown(); }

// ============================================================================
// init
// ============================================================================
bool ComputePathTracer::init(HWND hwnd, uint32_t width, uint32_t height) {
    VKR_LOG("============================================================");
    VKR_LOG("ComputePathTracer::init  hwnd=" << hwnd << "  " << width << "x" << height);
    VKR_LOG("============================================================");
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    VKR_LOG_STEP("0/14 prepareShaders");
    if (!prepareShaders())          { VKR_LOG_FAIL("prepareShaders"); return false; }

    scene_ = buildDefaultScene();
    camera_.yaw = 0.0f;
    camera_.pitch = 0.1f;
    camera_.distance = 3.0f;
    camera_.targetY = -1.0f;
    VKR_LOG("Scene: " << scene_.spheres.size() << " spheres, "
            << scene_.walls.size() << " walls, "
            << scene_.lights.size() << " lights");

    VKR_LOG_STEP("1/14 createInstance");
    if (!createInstance())            { VKR_LOG_FAIL("createInstance"); return false; }
    VKR_LOG_STEP("2/14 pickPhysicalDevice");
    if (!pickPhysicalDevice())        { VKR_LOG_FAIL("pickPhysicalDevice"); return false; }
    VKR_LOG_STEP("3/14 createSurface");
    if (!createSurface(hwnd))         { VKR_LOG_FAIL("createSurface"); return false; }
    VKR_LOG_STEP("4/14 createDeviceAndQueue");
    if (!createDeviceAndQueue())      { VKR_LOG_FAIL("createDeviceAndQueue"); return false; }
    VKR_LOG_STEP("5/14 createSwapchain");
    if (!createSwapchain())           { VKR_LOG_FAIL("createSwapchain"); return false; }
    VKR_LOG_STEP("6/14 createAccumImage");
    if (!createAccumImage())          { VKR_LOG_FAIL("createAccumImage"); return false; }
    VKR_LOG_STEP("7/14 createSceneBuffers");
    if (!createSceneBuffers())        { VKR_LOG_FAIL("createSceneBuffers"); return false; }
    VKR_LOG_STEP("8/14 createDescriptorPool");
    if (!createDescriptorPool())      { VKR_LOG_FAIL("createDescriptorPool"); return false; }
    VKR_LOG_STEP("9/14 createComputeLayout");
    if (!createComputeLayout())       { VKR_LOG_FAIL("createComputeLayout"); return false; }
    VKR_LOG_STEP("10/14 createPresentLayout");
    if (!createPresentLayout())       { VKR_LOG_FAIL("createPresentLayout"); return false; }
    VKR_LOG_STEP("11/14 createComputePipeline");
    if (!createComputePipeline())     { VKR_LOG_FAIL("createComputePipeline"); return false; }
    VKR_LOG_STEP("12/14 createPresentPipeline");
    if (!createPresentPipeline())     { VKR_LOG_FAIL("createPresentPipeline"); return false; }
    VKR_LOG_STEP("13/14 createCommandResources");
    if (!createCommandResources())    { VKR_LOG_FAIL("createCommandResources"); return false; }
    VKR_LOG_STEP("14/14 createFramebuffers");
    if (!createFramebuffers())        { VKR_LOG_FAIL("createFramebuffers"); return false; }

    VKR_LOG_OK("============================================================");
    VKR_LOG_OK("Vkray init complete. Ready to render.");
    VKR_LOG_OK("============================================================");
    return true;
}

bool ComputePathTracer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ZeroShell Vkray";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ZeroShell";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;
    VKR_LOG("  Requested API version: 1.2");

    // 枚举可用 validation layers
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
    VKR_LOG("  Available instance layers: " << layerCount);
    bool hasValidation = false;
    for (auto& l : availLayers) {
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            hasValidation = true;
            VKR_LOG("    Found VK_LAYER_KHRONOS_validation");
        }
    }

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = hasValidation ? 3 : 2;
    ci.ppEnabledExtensionNames = extensions;
    if (hasValidation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
        VKR_LOG("  Enabling validation layer for diagnostics");
    } else {
        VKR_LOG("  Validation layer not available");
    }

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    VKR_LOG("  vkCreateInstance = " << vr2s(r) << "  instance=" << instance_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateInstance failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Instance created");
    return true;
}

bool ComputePathTracer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    VKR_LOG("  Physical devices found: " << count);
    if (count == 0) {
        VKR_LOG_FAIL("No Vulkan GPU found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (uint32_t di = 0; di < count; di++) {
        auto dev = devices[di];

        // 打印设备信息
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        uint32_t apiMajor = VK_VERSION_MAJOR(props.apiVersion);
        uint32_t apiMinor = VK_VERSION_MINOR(props.apiVersion);
        VKR_LOG("  Device[" << di << "]: " << props.deviceName
                << "  API=" << apiMajor << "." << apiMinor
                << "  type=" << (int)props.deviceType
                << "  driver=" << VK_VERSION_MAJOR(props.driverVersion)
                << "." << VK_VERSION_MINOR(props.driverVersion));

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, qprops.data());

        bool hasGraphics = false, hasCompute = false;
        for (uint32_t i = 0; i < qcount; i++) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) hasGraphics = true;
            if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)  hasCompute  = true;
        }

        // 检查 swapchain 扩展支持
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchain = true;
                break;
            }
        }

        VKR_LOG("    hasGraphics=" << hasGraphics << " hasCompute=" << hasCompute
                << " hasSwapchain=" << hasSwapchain);

        if (hasGraphics && hasCompute && hasSwapchain) {
            physicalDevice_ = dev;
            VKR_LOG_OK("Selected device: " << props.deviceName
                       << " (API " << apiMajor << "." << apiMinor << ")");
            return true;
        }
    }
    VKR_LOG_FAIL("No suitable GPU found");
    return false;
}

bool ComputePathTracer::createDeviceAndQueue() {
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qcount, qprops.data());
    VKR_LOG("  Queue families: " << qcount);

    queueFamily_ = ~0u;
    for (uint32_t i = 0; i < qcount; i++) {
        bool g = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        bool c = (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &presentSupport);
        VKR_LOG("    QF[" << i << "] graphics=" << g << " compute=" << c
                << " present=" << (bool)presentSupport << " count=" << qprops[i].queueCount);
        if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            if (presentSupport) {
                queueFamily_ = i;
                break;
            }
        }
    }
    if (queueFamily_ == ~0u) {
        VKR_LOG("  No graphics+compute+present queue; falling back to graphics+compute");
        for (uint32_t i = 0; i < qcount; i++) {
            if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                queueFamily_ = i;
                break;
            }
        }
    }
    if (queueFamily_ == ~0u) {
        VKR_LOG_FAIL("No graphics+compute queue family");
        return false;
    }
    VKR_LOG("  Selected queue family: " << queueFamily_);

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queuePriority;

    // 启用 Vulkan 1.2 scalar block layout
    VkPhysicalDeviceVulkan12Features v12f{};
    v12f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12f.scalarBlockLayout = VK_TRUE;
    VKR_LOG("  Enabling Vulkan1.2 feature: scalarBlockLayout = VK_TRUE");

    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &v12f;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;

    VkResult r = vkCreateDevice(physicalDevice_, &dci, nullptr, &device_);
    VKR_LOG("  vkCreateDevice = " << vr2s(r) << "  device=" << device_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateDevice failed: " << vr2s(r));
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    VKR_LOG_OK("Device created, queue=" << queue_);
    return true;
}

bool ComputePathTracer::createSurface(HWND hwnd) {
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = GetModuleHandle(nullptr);
    ci.hwnd = hwnd;
    VkResult r = vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &surface_);
    VKR_LOG("  vkCreateWin32SurfaceKHR = " << vr2s(r) << "  surface=" << surface_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateWin32SurfaceKHR failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Surface created");
    return true;
}

bool ComputePathTracer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    VKR_LOG("  Surface caps: current=" << caps.currentExtent.width << "x" << caps.currentExtent.height
            << " min=" << caps.minImageExtent.width << "x" << caps.minImageExtent.height
            << " max=" << caps.maxImageExtent.width << "x" << caps.maxImageExtent.height
            << " minImage=" << caps.minImageCount << " maxImage=" << caps.maxImageCount);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, formats.data());
    VKR_LOG("  Surface formats: " << fmtCount);

    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    swapchainFormat_ = chosen.format;
    VKR_LOG("  Chosen format=" << chosen.format << " colorSpace=" << chosen.colorSpace);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == ~0u) {
        extent.width  = std::clamp(width_,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    swapchainExtent_ = extent;
    width_  = extent.width;
    height_ = extent.height;
    VKR_LOG("  Swapchain extent=" << extent.width << "x" << extent.height);

    uint32_t imageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }
    VKR_LOG("  Image count=" << imageCount);

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;

    VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_);
    VKR_LOG("  vkCreateSwapchainKHR = " << vr2s(r) << "  swapchain=" << swapchain_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateSwapchainKHR failed: " << vr2s(r));
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
    VKR_LOG("  Got " << imageCount << " swapchain images");

    swapchainViews_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = swapchainImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchainFormat_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        VkResult rr = vkCreateImageView(device_, &vci, nullptr, &swapchainViews_[i]);
        if (rr != VK_SUCCESS) {
            VKR_LOG_FAIL("vkCreateImageView (swapchain " << i << ") = " << vr2s(rr));
            return false;
        }
    }
    VKR_LOG_OK("Swapchain created with " << imageCount << " images");
    return true;
}

bool ComputePathTracer::createAccumImage() {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ici.extent = {width_, height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKR_LOG("  accum image: " << width_ << "x" << height_ << " R32G32B32A32_SFLOAT");

    VkResult r = vkCreateImage(device_, &ici, nullptr, &accumImage_);
    VKR_LOG("  vkCreateImage = " << vr2s(r));
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateImage (accum) failed: " << vr2s(r));
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, accumImage_, &req);
    VKR_LOG("  accum mem req: size=" << req.size << " alignment=" << req.alignment
            << " memTypeBits=0x" << std::hex << req.memoryTypeBits << std::dec);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKR_LOG("  accum memoryTypeIndex=" << ai.memoryTypeIndex);
    if (ai.memoryTypeIndex == ~0u) {
        VKR_LOG_FAIL("No DEVICE_LOCAL memory type for accum image");
        return false;
    }
    r = vkAllocateMemory(device_, &ai, nullptr, &accumMemory_);
    VKR_LOG("  vkAllocateMemory (accum) = " << vr2s(r));
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkAllocateMemory (accum) failed: " << vr2s(r));
        return false;
    }
    vkBindImageMemory(device_, accumImage_, accumMemory_, 0);

    // 两个 view：compute 写（GENERAL），graphics 采样（SHADER_READ_ONLY_OPTIMAL）
    auto makeView = [&](VkImageLayout) -> VkImageView {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = accumImage_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(device_, &vci, nullptr, &v);
        return v;
    };
    accumViewCompute_ = makeView(VK_IMAGE_LAYOUT_GENERAL);
    accumViewSample_  = makeView(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VKR_LOG("  accumViewCompute=" << accumViewCompute_ << " accumViewSample=" << accumViewSample_);
    if (!accumViewCompute_ || !accumViewSample_) {
        VKR_LOG_FAIL("Failed to create accum image views");
        return false;
    }
    VKR_LOG_OK("Accum image created");
    return true;
}

// 通用 buffer 创建辅助
static bool createBuffer(VkDevice dev, VkPhysicalDevice pdev,
                         VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props,
                         VkBuffer& buf, VkDeviceMemory& mem,
                         uint32_t (*findMem)(VkPhysicalDevice, uint32_t, VkMemoryPropertyFlags)) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMem(pdev, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == ~0u) return false;
    if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, buf, mem, 0);
    return true;
}

static uint32_t findMemThunk(VkPhysicalDevice pdev, uint32_t f, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((f & (1u << i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i;
    }
    return ~0u;
}

bool ComputePathTracer::createSceneBuffers() {
    VKR_LOG("  Camera UBO size=" << sizeof(CameraUBO));
    if (!createBuffer(device_, physicalDevice_, sizeof(CameraUBO),
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      cameraUbo_, cameraUboMem_, findMemThunk)) {
        VKR_LOG_FAIL("createBuffer (cameraUbo) failed");
        return false;
    }

    size_t sphereBytes = scene_.spheres.size() * sizeof(SphereGpu);
    VKR_LOG("  Sphere SSBO: " << scene_.spheres.size() << " x " << sizeof(SphereGpu)
            << " = " << sphereBytes << " bytes");
    if (!createBuffer(device_, physicalDevice_, sphereBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sphereBuf_, sphereBufMem_, findMemThunk)) {
        VKR_LOG_FAIL("createBuffer (sphere) failed");
        return false;
    }
    void* p = nullptr;
    vkMapMemory(device_, sphereBufMem_, 0, sphereBytes, 0, &p);
    memcpy(p, scene_.spheres.data(), sphereBytes);
    vkUnmapMemory(device_, sphereBufMem_);

    size_t wallBytes = scene_.walls.size() * sizeof(WallGpu);
    VKR_LOG("  Wall SSBO: " << scene_.walls.size() << " x " << sizeof(WallGpu)
            << " = " << wallBytes << " bytes");
    if (!createBuffer(device_, physicalDevice_, wallBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      wallBuf_, wallBufMem_, findMemThunk)) {
        VKR_LOG_FAIL("createBuffer (wall) failed");
        return false;
    }
    vkMapMemory(device_, wallBufMem_, 0, wallBytes, 0, &p);
    memcpy(p, scene_.walls.data(), wallBytes);
    vkUnmapMemory(device_, wallBufMem_);

    size_t lightBytes = scene_.lights.size() * sizeof(LightGpu);
    VKR_LOG("  Light SSBO: " << scene_.lights.size() << " x " << sizeof(LightGpu)
            << " = " << lightBytes << " bytes");
    if (!createBuffer(device_, physicalDevice_, lightBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      lightBuf_, lightBufMem_, findMemThunk)) {
        VKR_LOG_FAIL("createBuffer (light) failed");
        return false;
    }
    vkMapMemory(device_, lightBufMem_, 0, lightBytes, 0, &p);
    memcpy(p, scene_.lights.data(), lightBytes);
    vkUnmapMemory(device_, lightBufMem_);

    VKR_LOG("  Present UBO size=16");
    if (!createBuffer(device_, physicalDevice_, 16,
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      presentUbo_, presentUboMem_, findMemThunk)) {
        VKR_LOG_FAIL("createBuffer (presentUbo) failed");
        return false;
    }

    VKR_LOG_OK("Scene buffers created");
    return true;
}

bool ComputePathTracer::createDescriptorPool() {
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,      1 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,     2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,     3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 2;
    ci.poolSizeCount = 4;
    ci.pPoolSizes = poolSizes;
    VkResult r = vkCreateDescriptorPool(device_, &ci, nullptr, &descriptorPool_);
    VKR_LOG("  vkCreateDescriptorPool = " << vr2s(r) << "  pool=" << descriptorPool_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateDescriptorPool failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Descriptor pool created");
    return true;
}

bool ComputePathTracer::createComputeLayout() {
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 5;
    ci.pBindings = bindings;
    VkResult r = vkCreateDescriptorSetLayout(device_, &ci, nullptr, &computeSetLayout_);
    VKR_LOG("  vkCreateDescriptorSetLayout (compute, 5 bindings) = " << vr2s(r)
            << "  layout=" << computeSetLayout_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("createComputeLayout failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Compute descriptor set layout created");
    return true;
}

bool ComputePathTracer::createPresentLayout() {
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 2;
    ci.pBindings = bindings;
    VkResult r = vkCreateDescriptorSetLayout(device_, &ci, nullptr, &presentSetLayout_);
    VKR_LOG("  vkCreateDescriptorSetLayout (present, 2 bindings) = " << vr2s(r)
            << "  layout=" << presentSetLayout_);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("createPresentLayout failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Present descriptor set layout created");
    return true;
}

bool ComputePathTracer::createComputePipeline() {
    std::string dir = exeDir() + "\\spv\\";
    VKR_LOG("  SPV dir: " << dir);
    auto code = loadSpv(dir + "pathtracer.comp.spv");
    if (code.empty()) {
        VKR_LOG_FAIL("pathtracer.comp.spv is empty or missing");
        return false;
    }
    VKR_LOG("  pathtracer.comp.spv: " << code.size() << " dwords");

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size() * sizeof(uint32_t);
    smci.pCode = code.data();
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkResult r1 = vkCreateShaderModule(device_, &smci, nullptr, &module_);
    VKR_LOG("  vkCreateShaderModule (compute) = " << vr2s(r1) << "  module=" << module_);
    if (r1 != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateShaderModule failed: " << vr2s(r1));
        return false;
    }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &computeSetLayout_;
    VkResult r2 = vkCreatePipelineLayout(device_, &plci, nullptr, &computePipelineLayout_);
    VKR_LOG("  vkCreatePipelineLayout (compute) = " << vr2s(r2)
            << "  layout=" << computePipelineLayout_
            << "  setLayout=" << computeSetLayout_);
    if (r2 != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreatePipelineLayout failed: " << vr2s(r2));
        vkDestroyShaderModule(device_, module_, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = module_;
    pci.stage.pName = "main";
    pci.layout = computePipelineLayout_;
    VKR_LOG("  About to call vkCreateComputePipelines...");
    VKR_LOG("    pci.sType=" << pci.sType);
    VKR_LOG("    pci.stage.sType=" << pci.stage.sType);
    VKR_LOG("    pci.stage.stage=" << pci.stage.stage);
    VKR_LOG("    pci.stage.module=" << pci.stage.module);
    VKR_LOG("    pci.stage.pName=\"" << pci.stage.pName << "\"");
    VKR_LOG("    pci.layout=" << pci.layout);
    VKR_LOG("    device=" << device_ << " pipelineCache=VK_NULL_HANDLE");

    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &computePipeline_);
    VKR_LOG("  vkCreateComputePipelines = " << vr2s(r) << "  pipeline=" << computePipeline_);
    vkDestroyShaderModule(device_, module_, nullptr);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateComputePipelines failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Compute pipeline created!");
    return true;
}

bool ComputePathTracer::createPresentPipeline() {
    // Render pass
    VkAttachmentDescription att{};
    att.format = swapchainFormat_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    VkResult rpR = vkCreateRenderPass(device_, &rpci, nullptr, &presentRenderPass_);
    VKR_LOG("  vkCreateRenderPass = " << vr2s(rpR) << "  renderPass=" << presentRenderPass_);
    if (rpR != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateRenderPass failed: " << vr2s(rpR));
        return false;
    }

    std::string dir = exeDir() + "\\spv\\";
    auto vcode = loadSpv(dir + "present.vert.spv");
    auto fcode = loadSpv(dir + "present.frag.spv");
    if (vcode.empty() || fcode.empty()) {
        VKR_LOG_FAIL("present.vert.spv or present.frag.spv missing");
        return false;
    }

    auto makeModule = [&](const std::vector<uint32_t>& c) -> VkShaderModule {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = c.size() * sizeof(uint32_t);
        smci.pCode = c.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &smci, nullptr, &m);
        return m;
    };
    VkShaderModule vmod = makeModule(vcode);
    VkShaderModule fmod = makeModule(fcode);
    VKR_LOG("  present shader modules: vert=" << vmod << " frag=" << fmod);

    VkPipelineShaderStageCreateInfo stages[2];
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[0].pNext = nullptr;
    stages[0].pSpecializationInfo = nullptr;
    stages[0].flags = 0;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";
    stages[1].pNext = nullptr;
    stages[1].pSpecializationInfo = nullptr;
    stages[1].flags = 0;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &presentSetLayout_;
    VkResult plR = vkCreatePipelineLayout(device_, &plci, nullptr, &presentPipelineLayout_);
    VKR_LOG("  vkCreatePipelineLayout (present) = " << vr2s(plR)
            << "  layout=" << presentPipelineLayout_);
    if (plR != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreatePipelineLayout (present) failed: " << vr2s(plR));
        vkDestroyShaderModule(device_, vmod, nullptr);
        vkDestroyShaderModule(device_, fmod, nullptr);
        return false;
    }

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vs;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &ds;
    gpci.layout = presentPipelineLayout_;
    gpci.renderPass = presentRenderPass_;
    gpci.subpass = 0;

    VKR_LOG("  About to call vkCreateGraphicsPipelines...");
    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &presentPipeline_);
    VKR_LOG("  vkCreateGraphicsPipelines = " << vr2s(r) << "  pipeline=" << presentPipeline_);
    vkDestroyShaderModule(device_, vmod, nullptr);
    vkDestroyShaderModule(device_, fmod, nullptr);
    if (r != VK_SUCCESS) {
        VKR_LOG_FAIL("vkCreateGraphicsPipelines failed: " << vr2s(r));
        return false;
    }
    VKR_LOG_OK("Present pipeline created");
    return true;
}

bool ComputePathTracer::createCommandResources() {
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &cpci, nullptr, &commandPool_) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = commandPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cai, &commandBuffer_) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fci, nullptr, &frameFence_) != VK_SUCCESS) return false;

    // 每个交换链图像一个 renderFinished 信号量（避免 present 未完成时重用）
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_) != VK_SUCCESS) return false;
    renderFinished_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); i++) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]) != VK_SUCCESS) return false;
    }
    VKR_LOG("  Created 1 imageAvailable + " << swapchainImages_.size() << " renderFinished semaphores");

    // 分配 descriptor sets（一次分配两个）
    VkDescriptorSetLayout layouts[] = { computeSetLayout_, presentSetLayout_ };
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptorPool_;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device_, &dsai, sets) != VK_SUCCESS) return false;
    computeSet_ = sets[0];
    presentSet_ = sets[1];

    // 写 compute set
    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageView = accumViewCompute_;
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo camInfo{};
    camInfo.buffer = cameraUbo_;
    camInfo.range = sizeof(CameraUBO);

    VkDescriptorBufferInfo sphereInfo{};
    sphereInfo.buffer = sphereBuf_;
    sphereInfo.range = scene_.spheres.size() * sizeof(SphereGpu);

    VkDescriptorBufferInfo wallInfo{};
    wallInfo.buffer = wallBuf_;
    wallInfo.range = scene_.walls.size() * sizeof(WallGpu);

    VkDescriptorBufferInfo lightInfo{};
    lightInfo.buffer = lightBuf_;
    lightInfo.range = scene_.lights.size() * sizeof(LightGpu);

    VkWriteDescriptorSet writes[5];
    for (auto& w : writes) {
        w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = computeSet_;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &accumInfo;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &camInfo;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &sphereInfo;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].descriptorCount = 1;
    writes[3].pBufferInfo = &wallInfo;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].descriptorCount = 1;
    writes[4].pBufferInfo = &lightInfo;
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);

    // 写 present set
    VkSamplerCreateInfo sci2{};
    sci2.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci2.magFilter = VK_FILTER_LINEAR;
    sci2.minFilter = VK_FILTER_LINEAR;
    sci2.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci2.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(device_, &sci2, nullptr, &accumSampler_) != VK_SUCCESS) return false;

    VkDescriptorImageInfo sampleInfo{};
    sampleInfo.sampler = accumSampler_;
    sampleInfo.imageView = accumViewSample_;
    sampleInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo presentUboInfo{};
    presentUboInfo.buffer = presentUbo_;
    presentUboInfo.range = 16;

    VkWriteDescriptorSet pwrites[2];
    pwrites[0] = {};
    pwrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    pwrites[0].dstSet = presentSet_;
    pwrites[0].dstBinding = 0;
    pwrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pwrites[0].descriptorCount = 1;
    pwrites[0].pImageInfo = &sampleInfo;
    pwrites[1] = {};
    pwrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    pwrites[1].dstSet = presentSet_;
    pwrites[1].dstBinding = 1;
    pwrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pwrites[1].descriptorCount = 1;
    pwrites[1].pBufferInfo = &presentUboInfo;
    vkUpdateDescriptorSets(device_, 2, pwrites, 0, nullptr);

    return true;
}

bool ComputePathTracer::createFramebuffers() {
    presentFramebuffers_.resize(swapchainViews_.size());
    for (size_t i = 0; i < swapchainViews_.size(); i++) {
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = presentRenderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &swapchainViews_[i];
        fci.width = width_;
        fci.height = height_;
        fci.layers = 1;
        if (vkCreateFramebuffer(device_, &fci, nullptr, &presentFramebuffers_[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

// ============================================================================
// 渲染一帧
// ============================================================================
void ComputePathTracer::renderFrame() {
    vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &frameFence_);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        onResize(width_, height_);
        return;
    }
    if (r != VK_SUCCESS) return;

    // 更新 camera UBO
    CameraUBO camUbo = buildCameraUbo(camera_, width_, height_, frameCount_);
    void* p = nullptr;
    vkMapMemory(device_, cameraUboMem_, 0, sizeof(CameraUBO), 0, &p);
    memcpy(p, &camUbo, sizeof(CameraUBO));
    vkUnmapMemory(device_, cameraUboMem_);

    // 更新 present UBO (frameCount)
    uint32_t pc[4] = { frameCount_ + 1, 0, 0, 0 };  // +1 因为当前帧采样会被加进累积
    vkMapMemory(device_, presentUboMem_, 0, 16, 0, &p);
    memcpy(p, pc, 16);
    vkUnmapMemory(device_, presentUboMem_);

    vkResetCommandBuffer(commandBuffer_, 0);

    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &cbi);

    // === Barrier 1: accum image → GENERAL (compute 写) ===
    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.srcAccessMask = 0;
    b1.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b1.oldLayout = (frameCount_ == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = accumImage_;
    b1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commandBuffer_,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    // === Compute dispatch ===
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
        computePipelineLayout_, 0, 1, &computeSet_, 0, nullptr);
    uint32_t gx = (width_  + 15) / 16;
    uint32_t gy = (height_ + 15) / 16;
    vkCmdDispatch(commandBuffer_, gx, gy, 1);

    // === Barrier 2: accum image → SHADER_READ_ONLY_OPTIMAL (graphics 采样) ===
    VkImageMemoryBarrier b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = accumImage_;
    b2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commandBuffer_,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);

    // === Graphics: present pass ===
    VkClearValue clear{};
    clear.color = { {0, 0, 0, 1} };

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = presentRenderPass_;
    rpbi.framebuffer = presentFramebuffers_[imageIndex];
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = swapchainExtent_;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;

    vkCmdBeginRenderPass(commandBuffer_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
        presentPipelineLayout_, 0, 1, &presentSet_, 0, nullptr);

    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(width_);
    vp.height = static_cast<float>(height_);
    vp.minDepth = 0; vp.maxDepth = 1;
    vkCmdSetViewport(commandBuffer_, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);

    vkCmdDraw(commandBuffer_, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer_);

    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[imageIndex];

    vkQueueSubmit(queue_, 1, &si, frameFence_);
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;

    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        onResize(width_, height_);
    }

    if (frameCount_ < 5 || (frameCount_ % 60 == 0)) {
        VKR_LOG("frame " << frameCount_ << " presented, imageIndex=" << imageIndex
                << " eye=(" << camUbo.eye[0] << "," << camUbo.eye[1] << "," << camUbo.eye[2] << ")");
    }

    frameCount_++;
}

// ============================================================================
// Resize
// ============================================================================
void ComputePathTracer::onResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    vkDeviceWaitIdle(device_);

    // 销毁 swapchain 相关
    for (auto& fb : presentFramebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr), fb = VK_NULL_HANDLE;
    presentFramebuffers_.clear();
    for (auto& v : swapchainViews_) if (v) vkDestroyImageView(device_, v, nullptr), v = VK_NULL_HANDLE;
    swapchainViews_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr), swapchain_ = VK_NULL_HANDLE;

    // 销毁旧的 per-image renderFinished 信号量
    for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();

    // 销毁 accum
    if (accumViewCompute_) vkDestroyImageView(device_, accumViewCompute_, nullptr), accumViewCompute_ = VK_NULL_HANDLE;
    if (accumViewSample_)  vkDestroyImageView(device_, accumViewSample_,  nullptr), accumViewSample_  = VK_NULL_HANDLE;
    if (accumImage_) vkDestroyImage(device_, accumImage_, nullptr), accumImage_ = VK_NULL_HANDLE;
    if (accumMemory_) vkFreeMemory(device_, accumMemory_, nullptr), accumMemory_ = VK_NULL_HANDLE;

    width_ = width;
    height_ = height;

    if (!createSwapchain()) return;
    if (!createAccumImage()) return;
    if (!createFramebuffers()) return;

    // 重新创建 per-image renderFinished 信号量
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); i++) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]) != VK_SUCCESS) return;
    }
    VKR_LOG("  onResize: recreated " << renderFinished_.size() << " renderFinished semaphores");

    // 更新 compute set 的 accum 绑定
    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageView = accumViewCompute_;
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = computeSet_;
    w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.descriptorCount = 1;
    w.pImageInfo = &accumInfo;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    // 更新 present set 的 accum 绑定（复用 accumSampler_）
    VkDescriptorImageInfo sampleInfo{};
    sampleInfo.sampler = accumSampler_;
    sampleInfo.imageView = accumViewSample_;
    sampleInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w2{};
    w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w2.dstSet = presentSet_;
    w2.dstBinding = 0;
    w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w2.descriptorCount = 1;
    w2.pImageInfo = &sampleInfo;
    vkUpdateDescriptorSets(device_, 1, &w2, 0, nullptr);

    frameCount_ = 0;
}

// ============================================================================
// setCamera
// ============================================================================
void ComputePathTracer::setCamera(const OrbitCamera& cam) {
    camera_ = cam;
}

// ============================================================================
// shutdown
// ============================================================================
void ComputePathTracer::destroySwapchainObjects() {
    for (auto& fb : presentFramebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr), fb = VK_NULL_HANDLE;
    presentFramebuffers_.clear();
    for (auto& v : swapchainViews_) if (v) vkDestroyImageView(device_, v, nullptr), v = VK_NULL_HANDLE;
    swapchainViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr), swapchain_ = VK_NULL_HANDLE;
    if (accumViewCompute_) vkDestroyImageView(device_, accumViewCompute_, nullptr), accumViewCompute_ = VK_NULL_HANDLE;
    if (accumViewSample_)  vkDestroyImageView(device_, accumViewSample_,  nullptr), accumViewSample_  = VK_NULL_HANDLE;
    if (accumImage_) vkDestroyImage(device_, accumImage_, nullptr), accumImage_ = VK_NULL_HANDLE;
    if (accumMemory_) vkFreeMemory(device_, accumMemory_, nullptr), accumMemory_ = VK_NULL_HANDLE;
}

void ComputePathTracer::destroyAll() {
    if (device_) vkDeviceWaitIdle(device_);
    destroySwapchainObjects();

    if (accumSampler_) vkDestroySampler(device_, accumSampler_, nullptr), accumSampler_ = VK_NULL_HANDLE;

    if (presentPipeline_) vkDestroyPipeline(device_, presentPipeline_, nullptr), presentPipeline_ = VK_NULL_HANDLE;
    if (presentPipelineLayout_) vkDestroyPipelineLayout(device_, presentPipelineLayout_, nullptr), presentPipelineLayout_ = VK_NULL_HANDLE;
    if (presentRenderPass_) vkDestroyRenderPass(device_, presentRenderPass_, nullptr), presentRenderPass_ = VK_NULL_HANDLE;
    if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr), computePipeline_ = VK_NULL_HANDLE;
    if (computePipelineLayout_) vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr), computePipelineLayout_ = VK_NULL_HANDLE;

    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr), descriptorPool_ = VK_NULL_HANDLE;
    if (computeSetLayout_) vkDestroyDescriptorSetLayout(device_, computeSetLayout_, nullptr), computeSetLayout_ = VK_NULL_HANDLE;
    if (presentSetLayout_) vkDestroyDescriptorSetLayout(device_, presentSetLayout_, nullptr), presentSetLayout_ = VK_NULL_HANDLE;

    if (cameraUbo_)   vkDestroyBuffer(device_, cameraUbo_,   nullptr), cameraUbo_   = VK_NULL_HANDLE;
    if (cameraUboMem_)   vkFreeMemory(device_, cameraUboMem_,   nullptr), cameraUboMem_   = VK_NULL_HANDLE;
    if (sphereBuf_)   vkDestroyBuffer(device_, sphereBuf_,   nullptr), sphereBuf_   = VK_NULL_HANDLE;
    if (sphereBufMem_)   vkFreeMemory(device_, sphereBufMem_,   nullptr), sphereBufMem_   = VK_NULL_HANDLE;
    if (wallBuf_)     vkDestroyBuffer(device_, wallBuf_,     nullptr), wallBuf_     = VK_NULL_HANDLE;
    if (wallBufMem_)     vkFreeMemory(device_, wallBufMem_,     nullptr), wallBufMem_     = VK_NULL_HANDLE;
    if (lightBuf_)    vkDestroyBuffer(device_, lightBuf_,    nullptr), lightBuf_    = VK_NULL_HANDLE;
    if (lightBufMem_)    vkFreeMemory(device_, lightBufMem_,    nullptr), lightBufMem_    = VK_NULL_HANDLE;
    if (presentUbo_)  vkDestroyBuffer(device_, presentUbo_,  nullptr), presentUbo_  = VK_NULL_HANDLE;
    if (presentUboMem_)  vkFreeMemory(device_, presentUboMem_,  nullptr), presentUboMem_  = VK_NULL_HANDLE;

    for (auto s : renderFinished_) if (s) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();
    if (imageAvailable_) vkDestroySemaphore(device_, imageAvailable_, nullptr), imageAvailable_ = VK_NULL_HANDLE;
    if (frameFence_)     vkDestroyFence(device_, frameFence_,         nullptr), frameFence_     = VK_NULL_HANDLE;
    if (commandPool_)    vkDestroyCommandPool(device_, commandPool_,  nullptr), commandPool_    = VK_NULL_HANDLE;

    if (surface_)  vkDestroySurfaceKHR(instance_, surface_,  nullptr), surface_  = VK_NULL_HANDLE;
    if (device_)   vkDestroyDevice(device_, nullptr), device_   = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr), instance_ = VK_NULL_HANDLE;
}

void ComputePathTracer::shutdown() {
    destroyAll();
}

} // namespace vkray
