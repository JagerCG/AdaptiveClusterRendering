#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 1, binding = 0) uniform sampler2D shadowMap;
void main() { color = vec4(vec3(1 - texture(shadowMap, uv).r), 1); }
