#version 450

layout(location = 0) in vec3 position;

layout(push_constant) uniform DrawParams {
    mat4 viewProjection;
} drawParams;

void main()
{
    gl_Position = drawParams.viewProjection * vec4(position, 1.0);
}
