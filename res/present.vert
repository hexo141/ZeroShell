#version 460

// 全屏三角形（无 VBO，由 gl_VertexIndex 生成）
layout(location = 0) out vec2 vUV;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = pos * 2.0 - 1.0;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
