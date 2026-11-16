#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec3 surfaceNormal;
layout(location = 1) out vec3 worldPosition;

layout(push_constant) uniform DrawParams {
    mat4 viewProjection;
} drawParams;

void main()
{
    vec4 world = vec4(position, 1.0);
    gl_Position = drawParams.viewProjection * world;
    worldPosition = position;
    surfaceNormal = normal;
}
