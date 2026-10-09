#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out float outOcclusion;
layout(set = 0, binding = 0) uniform texture2D tDepth;
layout(set = 0, binding = 1) uniform texture2D tOcclusion;
layout(set = 0, binding = 128) uniform sampler sPoint;
layout(set = 0, binding = 256, std140) uniform OcclusionData
{
	mat4 Projection;
	mat4 InverseProjection;
	vec4 Parameters;
	vec4 Size;
} ao;
float ViewZ(vec2 uv, float depth)
{
	vec4 view = ao.InverseProjection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	return view.z / view.w;
}
void main()
{
	float depth = texture(sampler2D(tDepth, sPoint), vTexCoord).r;
	if (depth >= 1.0) { outOcclusion = 1.0; return; }
	float centerZ = ViewZ(vTexCoord, depth);
	float weighted = 0.0;
	float weights = 0.0;
	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			vec2 uv = vTexCoord + vec2(x,y) * ao.Size.xy;
			float sampleDepth = texture(sampler2D(tDepth, sPoint), uv).r;
			if (sampleDepth >= 1.0) { continue; }
			float dz = abs(ViewZ(uv, sampleDepth) - centerZ);
			float weight = exp(-float(x*x+y*y) / 8.0) * exp(-dz * 16.0 / ao.Parameters.x);
			weighted += texture(sampler2D(tOcclusion, sPoint), uv).r * weight;
			weights += weight;
		}
	}
	outOcclusion = weighted / max(weights, 0.00001);
}
