#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace vkray {

// ============================================================================
// 场景数据结构 — 与 GLSL SSBO 中的 std430 布局严格对应
// ============================================================================

// 材质类型
enum MaterialType : int32_t {
    MATERIAL_DIFFUSE  = 0,  // 漫反射（墙面、地面、顶面）
    MATERIAL_GLASS    = 1,  // 透明玻璃（反射 + 折射）
    MATERIAL_EMISSIVE = 2,  // 发光体（矩形灯光）
    MATERIAL_METAL    = 3,  // 金属镜面反射
};

// 球体：32 字节
struct SphereGpu {
    float cx, cy, cz;       // 中心
    float radius;           // 半径
    float ax, ay, az;       // 漫反射颜色（玻璃用 1,1,1）
    float ior;              // 折射率（<=0 表示非玻璃）
    int32_t material;       // MaterialType
    int32_t _pad[3];
};
static_assert(sizeof(SphereGpu) == 48, "SphereGpu layout");

// 平面墙：32 字节
// 平面方程: dot(normal, P) = dist
struct WallGpu {
    float nx, ny, nz;       // 法线（朝向房间内部）
    float dist;             // 距离原点
    float ax, ay, az;       // 漫反射颜色
    float _pad;
    int32_t material;
    int32_t _pad2[3];
};
static_assert(sizeof(WallGpu) == 48, "WallGpu layout");

// 矩形灯光：64 字节
// 由 corner + 两条边向量 u, v 定义
struct LightGpu {
    float cx, cy, cz;       // 角点
    float _pad1;
    float ux, uy, uz;       // 边向量 u
    float _pad2;
    float vx, vy, vz;       // 边向量 v
    float _pad3;
    float ex, ey, ez;       // 发光颜色 (linear, HDR)
    float intensity;        // 强度系数
    int32_t material;
    int32_t _pad4[3];
};
static_assert(sizeof(LightGpu) == 80, "LightGpu layout");

// 相机 UBO：64 字节（std140 布局）
struct CameraUBO {
    float eye[3];               // 视点
    float _pad0;
    float target[3];            // 目标点
    float _pad1;
    float up[3];                // 上方向
    float fov;                  // 视场角（弧度）
    uint32_t frameCount;        // 累积帧数
    uint32_t width;             // 渲染宽度
    uint32_t height;            // 渲染高度
    uint32_t samplePerFrame;    // 每帧采样数（保留，目前固定为 1）
};
static_assert(sizeof(CameraUBO) == 64, "CameraUBO layout");

// 球坐标相机参数（窗口输入 → CameraUBO）
struct OrbitCamera {
    float yaw = 0.0f;       // 水平角（弧度）
    float pitch = 0.2f;     // 俯仰角（弧度）
    float distance = 9.0f;  // 距目标距离
    float targetX = 0.0f;   // 目标点
    float targetY = 0.0f;
    float targetZ = 0.0f;
    float fov = 60.0f;      // 度
};

// ============================================================================
// 场景定义：粉紫双色墙壁房间 + 两枚金属球 + 顶部矩形灯光
// ============================================================================

struct Scene {
    std::vector<SphereGpu> spheres;
    std::vector<WallGpu> walls;
    std::vector<LightGpu> lights;
};

inline Scene buildDefaultScene() {
    Scene s;

    // --- 正方体房间 8x8x8（X x Y x Z），法线朝外，dist 为正 ---
    // 房间内壁范围: X in [-4, 4], Y in [-4, 4], Z in [-4, 4]
    // 相机朝 -Z 方向看（正面墙 = -Z 墙）

    // -Z 墙（正前方，白色）
    s.walls.push_back({0, 0, -1, 4,   1.0f, 0.0f, 0.0f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});
    // -X 墙（左侧，纯黄色）
    s.walls.push_back({-1, 0, 0, 4,   1.0f, 1.0f, 0.0f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});
    // +X 墙（右侧，纯蓝色）
    s.walls.push_back({ 1, 0, 0, 4,   0.0f, 0.0f, 1.0f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});
    // +Z 墙（正后方，纯绿色）
    s.walls.push_back({0, 0,  1, 4,   0.0f, 1.0f, 0.0f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});
    // -Y 地面（浅灰）
    s.walls.push_back({0, -1, 0, 4,   0.5f, 0.5f, 0.5f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});
    // +Y 天花板（深灰）
    s.walls.push_back({0,  1, 0, 4,   0.08f, 0.08f, 0.08f, 0.0f, MATERIAL_DIFFUSE, {0,0,0}});

    // --- 两枚金属镜面球（放在地面上，y = -4 + radius = -3）---
    // 球 A：银色金属，中心 (-1.2, -3, 0.5)，半径 1.0
    s.spheres.push_back({-1.2f, -3.0f, 0.5f, 1.0f,
                         0.9f, 0.9f, 0.9f, 0.0f,
                         MATERIAL_METAL, {0,0,0}});
    // 球 B：金色金属，中心 (1.3, -3, -0.3)，半径 1.0
    s.spheres.push_back({1.3f, -3.0f, -0.3f, 1.0f,
                         1.0f, 0.76f, 0.33f, 0.0f,
                         MATERIAL_METAL, {0,0,0}});

    // --- 天花板正方形光源（1x1，位于 Y=3.99 略低于天花板）---
    LightGpu light{};
    light.cx = -0.5f; light.cy = 3.99f; light.cz = -0.5f;
    light.ux = 1.0f;  light.uy = 0.0f;  light.uz = 0.0f;
    light.vx = 0.0f;  light.vy = 0.0f;  light.vz = 1.0f;
    light.ex = 1.0f;  light.ey = 1.0f;  light.ez = 1.0f;
    light.intensity = 3.0f;
    light.material = MATERIAL_EMISSIVE;
    s.lights.push_back(light);

    return s;
}

// 将球坐标相机参数转换为 CameraUBO（视点/上方向等）
inline CameraUBO buildCameraUbo(const OrbitCamera& cam, uint32_t width, uint32_t height, uint32_t frameCount) {
    CameraUBO ubo{};
    const float cp = cosf(cam.pitch), sp = sinf(cam.pitch);
    const float cy = cosf(cam.yaw),   sy = sinf(cam.yaw);
    // 球坐标 → 笛卡尔（Y up）
    ubo.eye[0] = cam.targetX + cam.distance * cp * sy;
    ubo.eye[1] = cam.targetY + cam.distance * sp;
    ubo.eye[2] = cam.targetZ + cam.distance * cp * cy;
    ubo.target[0] = cam.targetX;
    ubo.target[1] = cam.targetY;
    ubo.target[2] = cam.targetZ;
    ubo.up[0] = 0.0f; ubo.up[1] = 1.0f; ubo.up[2] = 0.0f;
    ubo.fov = cam.fov * 3.14159265358979323846f / 180.0f;
    ubo.frameCount = frameCount;
    ubo.width = width;
    ubo.height = height;
    ubo.samplePerFrame = 1;
    return ubo;
}

} // namespace vkray
