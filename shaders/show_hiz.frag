#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(std430,set=1,binding=0) readonly buffer Pyramid { float z[]; } pyramid;
layout(push_constant) uniform Params { uvec4 mip; } p;
void main() {
    uvec2 xy = min(uvec2(clamp(uv, 0.0, 1.0) * p.mip.xy), p.mip.xy - 1u);
    color = vec4(vec3(1.0 - pyramid.z[p.mip.z + xy.y * p.mip.x + xy.x]), 1);
}
