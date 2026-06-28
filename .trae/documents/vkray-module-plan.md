# Vkray 模块实施计划

## 摘要

新增 `vkray` 模块，提供 `vkray` 命令启动一个独立 Win32 窗口，使用 Vulkan **计算着色器（Compute Shader）路径追踪**渲染一个粉紫双色墙壁的简约房间，房间白色地面上静置两枚透明玻璃球，球体清晰折射周围彩色墙面与顶部矩形灯光。所有 SPIR-V 着色器由 Vulkan SDK 的 `glslangValidator` 在构建期生成，**禁止手写 hex/字节码**。当前实现走 GPU 计算路径，通过 `IRenderer` 抽象接口为未来切换到 `VK_KHR_ray_tracing_pipeline` 硬件光追保留扩展点（本期不实现硬件光追）。

---

## 当前状态分析

### 项目架构
- **模块基类**：[include/module.h](file:///e:/Visual%20Studio%20Repos/ZeroShell/include/module.h) — 抽象类 `Module`，接口含 `name()`/`description()`/`getCommands()`/`execute()`/`init()`/`shutdown()`
- **模块注册**：[src/shell.cpp](file:///e:/Visual%20Studio%20Repos/ZeroShell/src/shell.cpp#L36-L72) 的 `Shell::Shell()` 构造函数中通过 `registry_->registerModule(std::make_unique<XxxModule>())` 注册
- **构建系统**：Visual Studio `.vcxproj`，C++20，PlatformToolset `v145`，含 Win32/x64 + Debug/Release 四套配置
- **现有 Win32 窗口模式参考**：[src/modules/ciallo_module.cpp](file:///e:/Visual%20Studio%20Repos/ZeroShell/src/modules/ciallo_module.cpp) — 在 `std::thread(danmakuThread).detach()` 中创建 `CreateWindowExW`，自带消息循环，再次输入命令时 `PostMessage(WM_CLOSE)` 关闭
- **Vulkan SDK 使用惯例**：[third_party/imgui/examples/example_win32_vulkan/example_win32_vulkan.vcxproj](file:///e:/Visual%20Studio%20Repos/ZeroShell/third_party/imgui/examples/example_win32_vulkan/example_win32_vulkan.vcxproj) — 通过 `%VULKAN_SDK%\include` 和 `%VULKAN_SDK%\lib` 引用，链接 `vulkan-1.lib`

### 关键背景
- 用户的 `VkTest` 应用曾尝试使用 `vkCreateRayTracingPipelinesKHR` 硬件光追管线，在 RTX 3060 上返回 `VK_ERROR_INITIALIZATION_FAILED (-3)` 失败。因此本期明确使用 **GPU 计算路径**（compute shader path tracing），不依赖 `VK_KHR_ray_tracing_pipeline`，回避硬件光追管线的兼容性问题。

### 用户决策
1. 相机交互：**鼠标轨道交互**（左键拖拽旋转、滚轮缩放、右键平移）
2. RT 可选钩子：**抽象渲染器接口**（定义 `IRenderer`，当前实现 `ComputePathTracer`，未来新增 `HardwareRayTracer` 作为兄弟实现，命令行暂不暴露开关）
3. 不使用 imgui，使用 Win32 API 创建窗口

---

## 设计方案

### 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│  ZeroShell.exe (VkrayModule)                                │
│                                                             │
│  vkray 命令 ──→ detached 线程 ──→ VkrayWindow 消息循环       │
│                                  ↓                           │
│                                  VkrayRenderer (IRenderer)   │
│                                  ↓                           │
│                                  ComputePathTracer 实现       │
│                                     │                        │
│              ┌──────────────────────┼─────────────────────┐  │
│              │                      │                     │  │
│         Vulkan Instance      Win32 Surface           Compute  │
│         + Device             + Swapchain             Pipeline │
│                                                       (GLSL)  │
└─────────────────────────────────────────────────────────────┘
                          │
                          ↓ (构建期)
            glslangValidator -V *.comp/*.vert/*.frag → *.spv
            （由 Vulkan SDK 生成，不手写 hex）
```

### 渲染管线（纯 Compute 路径）

1. **计算阶段**（`pathtracer.comp`）：每个 workgroup 16×16 像素，对每个像素生成相机主光线，进行路径追踪：
   - 与房间盒体（6 面墙）求交 — 解析 AABB
   - 与 2 枚玻璃球求交 — 解析球体
   - 玻璃材质：Snell 折射 + Schlick 菲涅尔反射，俄罗斯轮盘赌决定反射/折射
   - 墙面材质：漫反射 BRDF + 余弦加权采样
   - 矩形顶光：显式采样（Next Event Estimation）
   - 累积到 RGBA32F 存储 image，每帧 +1 sample
2. **呈现阶段**（`present.vert` + `present.frag`）：渲染全屏三角形，采样累积 image，除以帧数，做 ACES Filmic Tonemapping + Gamma，输出到 swapchain image（B8G8R8A8_UNORM）
3. **累积重置**：相机移动时帧计数归零，重新累积（避免拖影）

### 场景描述

- **房间**：内壁尺寸 8×5×8（X×Y×Z）的盒体，内表面朝内
  - +X 墙：粉色 `#FFB3D9`
  - -X 墙：紫色 `#9B59B6`
  - +Z / -Z 墙：粉紫渐变（按 Y 插值）
  - 地面（-Y）：白色 `#F5F5F5`，漫反射
  - 顶面（+Y）：深灰 `#202020`，中央开矩形灯光孔
- **矩形灯光**：顶面中央 3×2（X×Z）的发射面，发光颜色暖白 `#FFF5E1`，强度 ~30
- **两枚玻璃球**：
  - 球 A：中心 `(-1.2, -1.5, 0.5)`，半径 1.0
  - 球 B：中心 `(1.3, -1.2, -0.3)`，半径 1.3
  - 材质：IOR = 1.5（玻璃），无色透明，全镜面反射/折射

### Vulkan 资源清单

| 资源 | 类型 | 用途 |
|------|------|------|
| Instance | VkInstance | 启用 `VK_KHR_surface` + `VK_KHR_win32_surface` |
| PhysicalDevice | VkPhysicalDevice | 选择首个支持 compute + graphics 的 GPU |
| Device | VkDevice | 启用 swapchain |
| Queue | VkQueue | graphics + compute 共用一族 |
| Surface | VkSurfaceKHR | Win32 平台表面 |
| Swapchain | VkSwapchainKHR | B8G8R8A8_UNORM，双缓冲 |
| AccumImage | VkImage (R32G32B32A32_SFLOAT) | 路径追踪累积缓冲（storage image） |
| SceneUBO | VkBuffer | 相机参数 + 帧数 + 采样参数 |
| SceneSSBO | VkBuffer | 球体数组 + 墙面数组 + 灯光数组 |
| ComputePipeline | VkPipeline | 绑定 `pathtracer.comp.spv` |
| GraphicsPipeline | VkPipeline | 全屏三角形 present pass |
| DescriptorSet ×2 | VkDescriptorSet | compute 用 / graphics 用 |

### Win32 窗口

- 类名 `ZeroShellVkrayClass`，独立消息循环在 detached 线程
- 窗口尺寸 1280×720，可调整大小（触发 swapchain 重建）
- 输入：`WM_LBUTTONDOWN/WM_MOUSEMOVE` 旋转、`WM_MOUSEWHEEL` 缩放、`WM_RBUTTONDOWN/WM_MOUSEMOVE` 平移
- `WM_CLOSE` → 销毁窗口 → 退出消息循环 → 清理 Vulkan 资源 → 线程退出
- 再次输入 `vkray` 命令时 `PostMessage(g_hWnd, WM_CLOSE, 0, 0)`（同 ciallo 模式）

---

## 文件清单

### 新建文件

| 文件 | 说明 |
|------|------|
| `src/modules/vkray_module.h` | `VkrayModule` 类声明（继承 `Module`） |
| `src/modules/vkray_module.cpp` | 模块实现：命令路由、窗口线程启停、`g_hWnd` 全局状态 |
| `src/modules/vkray/vkray_window.h` | Win32 窗口封装：注册类、创建窗口、消息循环、输入状态 |
| `src/modules/vkray/vkray_window.cpp` | 窗口实现 + 鼠标输入 → 相机参数 |
| `src/modules/vkray/vkray_renderer.h` | `IRenderer` 抽象接口 + `ComputePathTracer` 声明 |
| `src/modules/vkray/vkray_renderer.cpp` | Vulkan 初始化、swapchain、compute/graphics 管线、帧循环 |
| `src/modules/vkray/vkray_scene.h` | 场景数据结构（球、墙、灯光）+ UBO/SSBO 布局 |
| `src/modules/vkray/shaders/pathtracer.comp` | GLSL 计算着色器：路径追踪主循环 |
| `src/modules/vkray/shaders/present.vert` | GLSL 顶点着色器：全屏三角形 |
| `src/modules/vkray/shaders/present.frag` | GLSL 片段着色器：tonemap + 输出 |
| `src/modules/vkray/compile_shaders.bat` | 构建期脚本：调用 `glslangValidator -V` 生成 .spv 到 `$(OutDir)spv\` |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/shell.cpp` | 添加 `#include "modules/vkray_module.h"` 和 `registry_->registerModule(std::make_unique<VkrayModule>());` |
| `ZeroShell.vcxproj` | 1) 新增 `ClCompile`/`ClInclude` 项；2) `AdditionalIncludeDirectories` 追加 `%VULKAN_SDK%\include`；3) `AdditionalLibraryDirectories` 追加 `%VULKAN_SDK%\lib`（x64）/`%VULKAN_SDK%\lib32`（Win32）；4) `AdditionalDependencies` 追加 `vulkan-1.lib`；5) `<PreBuildEvent>` 调用 `src\modules\vkray\compile_shaders.bat $(OutDir)` |
| `ZeroShell.vcxproj.filters` | 新增 `模块\vkray` 和 `模块\vkray\shaders` 过滤器，归类新文件 |

---

## 详细实现要点

### 1. `vkray_module.h` / `vkray_module.cpp`

```cpp
// vkray_module.h
#pragma once
#include "module.h"
#include <windows.h>
#include <mutex>

class VkrayModule : public Module {
public:
    const char* name() const override { return "VkrayModule"; }
    const char* description() const override { return "Vulkan GPU compute path tracing demo (independent window)"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
```

实现要点：
- `getCommands()` 返回 `{ "vkray" }`
- `execute("vkray", args)`：
  - 加锁 `g_wndMutex`
  - 若 `g_hWnd` 非空 → `PostMessage(g_hWnd, WM_CLOSE, 0, 0)`，打印 `vkray stopped.`
  - 否则 `std::thread(vkrayMainThread).detach()`，打印启动提示
- 全局 `HWND g_hWnd` + `std::mutex g_wndMutex`（同 ciallo 模式）

### 2. `vkray_window.h` / `vkray_window.cpp`

- `vkrayMainThread()`：
  1. `RegisterClassExW`（类名 `ZeroShellVkrayClass`，`lpfnWndProc = VkrayWndProc`）
  2. `CreateWindowExW` 创建 1280×720 普通窗口（非分层，非 topmost）
  3. 创建 `VkrayRenderer` 实例，传入 HWND
  4. `ShowWindow` + 进入消息循环
  5. 每帧 `PeekMessage`，无消息时调用 `renderer->renderFrame()`
  6. `WM_DESTROY` 时 `renderer.reset()` → `PostQuitMessage`
- `VkrayWndProc` 处理：
  - `WM_LBUTTONDOWN` / `WM_MOUSEMOVE`：左键拖拽 → 更新 yaw/pitch
  - `WM_RBUTTONDOWN` / `WM_MOUSEMOVE`：右键拖拽 → 更新 target 偏移
  - `WM_MOUSEWHEEL`：缩放距离
  - `WM_SIZE`：标记 swapchain 需重建
  - `WM_CLOSE` / `WM_DESTROY`：清理
- 相机参数（球坐标系）：`yaw`, `pitch`, `distance`, `target`；任意变化时通知 renderer 重置累积

### 3. `vkray_renderer.h` / `vkray_renderer.cpp`

```cpp
// vkray_renderer.h
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(HWND hwnd, uint32_t width, uint32_t height) = 0;
    virtual void renderFrame() = 0;
    virtual void onResize(uint32_t width, uint32_t height) = 0;
    virtual void onCameraChanged() = 0;  // 重置累积
    virtual void shutdown() = 0;
};

class ComputePathTracer : public IRenderer {
    // Vulkan 句柄成员...
public:
    bool init(HWND hwnd, uint32_t w, uint32_t h) override;
    void renderFrame() override;
    void onResize(uint32_t w, uint32_t h) override;
    void onCameraChanged() override { frameCount_ = 0; }
    void shutdown() override;

    // 由 window 调用，设置最新相机参数
    void setCamera(const Camera& cam);
};
```

实现要点：
- `init()`：
  1. `vkCreateInstance`（启用 swapchain/surface/win32_surface 扩展）
  2. 选 physical device（评分：支持 graphics+compute 队列 + swapchain）
  3. `vkCreateDevice`（启用 swapchain）
  4. `vkCreateWin32SurfaceKHR`
  5. 选 queue family（graphics|compute）
  6. 创建 swapchain（B8G8R8A8_UNORM，FIFO，2 image）
  7. 创建累积 storage image（R32G32B32A32_SFLOAT）
  8. 创建 SceneUBO + SceneSSBO（写死场景数据）
  9. 加载 3 个 .spv 文件（从 `<exe_dir>/spv/` 读取，路径同 ciallo `getResPath` 模式但指向 `spv/`）
  10. 创建 compute pipeline + graphics pipeline + descriptor sets
  11. 创建 command pool + command buffer
- `renderFrame()`：
  1. `vkAcquireNextImageKHR`
  2. 录 command buffer：
     - Image memory barrier 让 swapchain image 转 `COLOR_ATTACHMENT_OPTIMAL`、accum image 转 `GENERAL`
     - `vkCmdBindPipeline(compute)` + `vkCmdBindDescriptorSets` + `vkCmdDispatch((W+15)/16, (H+15)/16, 1)`
     - Barrier 让 accum image 转 `SHADER_READ_ONLY_OPTIMAL`、swapchain image 转 `COLOR_ATTACHMENT_OPTIMAL`
     - `vkCmdBindPipeline(graphics)` + `vkCmdBindDescriptorSets` + `vkCmdDraw(3, 1, 0, 0)`
     - Barrier 让 swapchain image 转 `PRESENT_SRC_KHR`
  3. `vkQueueSubmit` + `vkQueuePresentKHR`
  4. `frameCount_++`
- `onResize()`：等设备空闲 → 销毁旧 swapchain + accum image → 重建
- `shutdown()`：`vkDeviceWaitIdle` → 销毁所有资源 → `vkDestroyDevice` → `vkDestroySurfaceKHR` → `vkDestroyInstance`
- 加载 .spv：`std::ifstream` 读二进制，返回 `std::vector<uint32_t>`

### 4. `vkray_scene.h`

```cpp
struct Sphere { glm::vec4 center_radius; glm::vec4 albedo_ior; int material; ... };
struct Wall   { glm::vec4 normal_dist;   glm::vec4 albedo;     int material; ... };
struct AreaLight { glm::vec4 corner_u; glm::vec4 corner_v; glm::vec4 corner_w; glm::vec3 emission; ... };
struct CameraUBO { glm::vec3 eye; glm::vec3 target; glm::vec3 up; float fov; uint32_t frameCount; uint32_t width; uint32_t height; uint32_t samplePerFrame; };

// SSBO 布局：sphere[2] + wall[6] + light[1]，固定大小
```

注：使用 `glm`（需在 `AdditionalIncludeDirectories` 加 `third_party/` 或将 glm 头文件加入）。若项目无 glm，则手写 `vec3`/`vec4` POD 结构（仅 4 浮点数，不引入依赖）。**优先选择手写 POD**，避免新增第三方依赖。

### 5. 着色器（GLSL，由 SDK 编译为 SPIR-V）

`pathtracer.comp` 关键结构：
```glsl
#version 460
layout(local_size_x = 16, local_size_y = 16) in;
layout(set=0, binding=0, rgba32f) uniform image2D accumImg;
layout(set=0, binding=1) uniform CameraUBO { vec3 eye; vec3 target; vec3 up; float fov; uint frameCount; uint width; uint height; uint samplePerFrame; } cam;
layout(set=0, binding=2) readonly buffer SphereBuf { vec4 center_radius[]; vec4 albedo_ior[]; int material[]; } spheres;
layout(set=0, binding=3) readonly buffer WallBuf { ... } walls;
layout(set=0, binding=4) readonly buffer LightBuf { ... } lights;

// PRNG (PCG hash), 相机光线生成, 球/AABB 求交, 路径追踪循环（最多 8 反弹）,
// 菲涅尔+折射, 漫反射采样, 显式灯光采样, 累积写入 accumImg
```

`present.vert`：全屏三角形（无 VBO，gl_VertexIndex 生成）
`present.frag`：`texture(accumImg, uv) / frameCount` → ACES tonemap → gamma → `outColor`

### 6. `compile_shaders.bat`

```bat
@echo off
setlocal
set OUT_DIR=%1
if "%VULKAN_SDK%"=="" (
    echo [vkray] VULKAN_SDK not set, skipping shader compilation
    exit /b 0
)
set GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe
set SRC=%~dp0shaders
set DST=%OUT_DIR%spv
if not exist "%DST%" mkdir "%DST%"
"%GLSLANG%" -V "%SRC%\pathtracer.comp" -o "%DST%\pathtracer.comp.spv"
"%GLSLANG%" -V "%SRC%\present.vert"   -o "%DST%\present.vert.spv"
"%GLSLANG%" -V "%SRC%\present.frag"   -o "%DST%\present.frag.spv"
exit /b 0
```

调用方式：在 `ZeroShell.vcxproj` 每个 `<ItemDefinitionGroup>` 内加：
```xml
<PreBuildEvent>
  <Command>call "$(ProjectDir)src\modules\vkray\compile_shaders.bat" "$(OutDir)"</Command>
</PreBuildEvent>
```

---

## 假设与决策

1. **Vulkan SDK 可用性**：假设用户环境已安装 Vulkan SDK 且 `%VULKAN_SDK%` 环境变量已设置（与 imgui 示例项目假设一致）。若未设置，pre-build 脚本静默跳过，运行时会因 .spv 缺失报错。
2. **GPU 计算路径**：仅使用核心 Vulkan 1.2 + swapchain/surface/win32_surface 扩展，不启用任何 `VK_KHR_ray_tracing_*` 扩展。
3. **RT 可选钩子**：定义 `IRenderer` 抽象基类，本期仅实现 `ComputePathTracer`。未来新增 `HardwareRayTracer : public IRenderer` 即可，无需改动窗口/模块代码。
4. **不使用 imgui**：窗口、UI、输入全部走 Win32 API。
5. **不使用 glm**：着色器 UBO/SSBO 使用手写 POD（4×float 数组），避免新增第三方依赖。
6. **窗口模式**：独立窗口在 detached 线程运行，再次输入 `vkray` 命令关闭（复用 ciallo 模式）。
7. **着色器路径**：.spv 文件输出到 `$(OutDir)spv\`，运行时从 `<exe_dir>/spv/` 加载。
8. **场景固定**：房间、球体、灯光位置在 SSBO 中硬编码，不提供运行时编辑。
9. **累积渲染**：相机静止时持续累积降噪，相机移动时立即重置。
10. **平台**：仅支持 x64（Vulkan 计算在 Win32 意义不大，且 ciallo/TimeHack 等模块的复杂图形功能也以 x64 为主）。Win32 配置仍保留编译通过（vulkan-1.lib 路径用 `%VULKAN_SDK%\lib32`），但渲染效果以 x64 为准。

---

## 验证步骤

1. 确认 `VULKAN_SDK` 环境变量已设置（`echo %VULKAN_SDK%`）
2. 在 Visual Studio 中重新生成 ZeroShell（x64 Debug）
   - 检查 pre-build 输出：`glslangValidator` 生成 3 个 .spv 到 `x64/Debug/spv/`
   - 检查编译无错误
3. 运行 ZeroShell，输入 `vkray`
   - 应弹出独立 1280×720 窗口，标题 `ZeroShell Vulkan Ray Tracing`
4. 验证渲染效果：
   - 房间四壁为粉/紫色，地面白色
   - 两枚透明玻璃球可见，球内可见折射的彩色墙面与顶部矩形灯光
   - 静止时画面逐帧降噪（累积）
5. 验证交互：
   - 左键拖拽：相机绕房间旋转，画面立即重置累积
   - 滚轮：拉近/拉远
   - 右键拖拽：平移视角中心
   - 调整窗口大小：swapchain 自动重建，画面继续渲染
6. 在 ZeroShell 中再次输入 `vkray`：窗口应关闭，控制台打印 `vkray stopped.`
7. 关闭窗口后再次输入 `vkray`：应能重新启动
8. 退出 ZeroShell：无崩溃、无资源泄漏（任务管理器观察 GPU 内存释放）
