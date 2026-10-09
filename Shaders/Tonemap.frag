#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform texture2D tHdr;
layout(set = 0, binding = 128) uniform sampler sHdr;
layout(push_constant) uniform ToneData
{
	vec4 Parameters;
} tone;
vec3 LinearToSrgb(vec3 linearColor)
{
	return mix(12.92 * linearColor, 1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055, greaterThan(linearColor, vec3(0.0031308)));
}
void main()
{
	vec3 hdr = max(texture(sampler2D(tHdr, sHdr), vTexCoord).rgb * tone.Parameters.x, vec3(0.0));
	vec3 mapped = clamp((hdr * (2.51 * hdr + 0.03)) / (hdr * (2.43 * hdr + 0.59) + 0.14), 0.0, 1.0);
	outColor = vec4(LinearToSrgb(mapped), 1.0);
}
