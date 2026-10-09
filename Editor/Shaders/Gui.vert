#version 450
layout(location=0) in vec2 Position;
layout(location=1) in vec2 TexCoord;
layout(location=2) in vec4 Color;
layout(binding=256) uniform Constants { vec2 Scale; vec2 Translate; } Draw;
layout(location=0) out vec2 UV;
layout(location=1) out vec4 Tint;
void main()
{
    UV = TexCoord;
    Tint = Color;
    gl_Position = vec4(Position * Draw.Scale + Draw.Translate, 0.0, 1.0);
}
