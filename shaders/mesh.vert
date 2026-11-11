#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec3 surfaceNormal;

layout(push_constant) uniform DrawParams {
    mat4 viewProjection;
} drawParams;

void main()
{
    gl_Position = drawParams.viewProjection * vec4(position, 1.0);
    surfaceNormal = normal;
}
