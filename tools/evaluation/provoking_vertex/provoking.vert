#version 450
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 0) flat out vec4 vertexColor;

void main()
{
    gl_Position = inPosition;
    vertexColor = inColor;
}
