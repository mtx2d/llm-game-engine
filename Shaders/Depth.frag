#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec2 vTexCoord1;
layout(location = 2) in vec4 vColor;
layout(set = 0, binding = 257, std140) uniform MaterialData
{
	vec4 BaseColor;
	vec4 EmissiveMetallic;
	vec4 Parameters;
	vec4 Transforms[15];
	vec4 TextureInfo[5];
} material;
layout(set = 0, binding = 0) uniform texture2D tBaseColor;
layout(set = 0, binding = 128) uniform sampler sBaseColor;
void main()
{
	if (material.Parameters.z > 0.5 && material.Parameters.z < 1.5)
	{
		vec2 uv = material.TextureInfo[0].x < 0.5 ? vTexCoord : vTexCoord1;
		uv = (mat3(material.Transforms[0].xyz, material.Transforms[1].xyz, material.Transforms[2].xyz) * vec3(uv, 1.0)).xy;
		float alpha = texture(sampler2D(tBaseColor, sBaseColor), uv).a * material.BaseColor.a * vColor.a;
		if (alpha < material.Parameters.y) { discard; }
	}
}
