#version 450
layout(binding=0) uniform texture2D Font;
layout(binding=128) uniform sampler FontSampler;
layout(location=0) in vec2 UV;
layout(location=1) in vec4 Tint;
layout(location=0) out vec4 OutputColor;
void main()
{
    OutputColor = Tint * texture(sampler2D(Font, FontSampler), UV);
}
