#version 450
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput mainDepth;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(vec3(1.0 - subpassLoad(mainDepth).r), 1.0);
}
