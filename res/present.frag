#version 460

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uAccum;

layout(set = 0, binding = 1, std140) uniform PresentUBO {
    uint frameCount;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} present;

layout(location = 0) out vec4 outColor;

// ACES Filmic Tonemapping
vec3 acesTonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 accum = texture(uAccum, vUV * 0.5 + 0.5).rgb;
    float count = max(1.0, float(present.frameCount));
    vec3 avgColor = accum / count;

    vec3 tonemapped = acesTonemap(avgColor);
    vec3 gamma = pow(tonemapped, vec3(1.0 / 2.2));

    outColor = vec4(gamma, 1.0);
}
