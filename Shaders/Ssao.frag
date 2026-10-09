#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out float outOcclusion;
layout(set = 0, binding = 0) uniform texture2D tDepth;
layout(set = 0, binding = 128) uniform sampler sPoint;
layout(set = 0, binding = 256, std140) uniform OcclusionData
{
	mat4 Projection;
	mat4 InverseProjection;
	vec4 Parameters;
	vec4 Size;
} ao;
vec3 ViewPosition(vec2 uv, float depth)
{
	vec4 view = ao.InverseProjection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	return view.xyz / view.w;
}
vec3 SamplePosition(vec2 uv)
{
	return ViewPosition(uv, texture(sampler2D(tDepth, sPoint), uv).r);
}
void main()
{
	float depth = texture(sampler2D(tDepth, sPoint), vTexCoord).r;
	if (depth >= 1.0) { outOcclusion = 1.0; return; }
	vec3 position = ViewPosition(vTexCoord, depth);
	vec3 right = SamplePosition(vTexCoord + vec2(ao.Size.x, 0.0)) - position;
	vec3 left = position - SamplePosition(vTexCoord - vec2(ao.Size.x, 0.0));
	vec3 down = SamplePosition(vTexCoord + vec2(0.0, ao.Size.y)) - position;
	vec3 up = position - SamplePosition(vTexCoord - vec2(0.0, ao.Size.y));
	vec3 dx = abs(left.z) < abs(right.z) ? left : right;
	vec3 dy = abs(up.z) < abs(down.z) ? up : down;
	vec3 normal = cross(dy, dx);
	float lengthSquared = dot(normal, normal);
	normal = lengthSquared > 0.000000000001 ? normal * inversesqrt(lengthSquared) : vec3(0, 0, 1);
	if (dot(normal, -position) < 0.0) { normal = -normal; }
	ivec2 cell = ivec2(gl_FragCoord.xy) & ivec2(3);
	float angle = float(cell.x * 4 + cell.y) * 2.39996323;
	vec3 randomDirection = vec3(cos(angle), sin(angle), 0.0);
	vec3 tangent = randomDirection - normal * dot(randomDirection, normal);
	if (dot(tangent, tangent) < 0.00001) { tangent = cross(normal, vec3(0,1,0)); }
	tangent = normalize(tangent);
	mat3 basis = mat3(tangent, cross(normal, tangent), normal);
	float obstruction = 0.0;
	for (int index = 0; index < 32; ++index)
	{
		float fraction = (float(index) + 0.5) / 32.0;
		float phi = float(index) * 2.39996323;
		float z = 0.1 + 0.9 * fraction;
		float radial = sqrt(max(0.0, 1.0 - z * z));
		// Permute sample lengths so distant and nearby samples cover the full hemisphere.
		float scale = 0.1 + 0.9 * pow(float((index * 13) % 32 + 1) / 32.0, 2.0);
		vec3 samplePosition = position + basis * vec3(cos(phi) * radial, sin(phi) * radial, z) * ao.Parameters.x * scale;
		vec4 projected = ao.Projection * vec4(samplePosition, 1.0);
		if (projected.w <= 0.0) { continue; }
		vec2 uv = vec2(projected.x / projected.w * 0.5 + 0.5, 0.5 - projected.y / projected.w * 0.5);
		if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1)))) { continue; }
		float surfaceDepth = texture(sampler2D(tDepth, sPoint), uv).r;
		if (surfaceDepth >= 1.0) { continue; }
		float surfaceZ = ViewPosition(uv, surfaceDepth).z;
		float rangeWeight = smoothstep(0.0, 1.0, ao.Parameters.x / max(abs(position.z - surfaceZ), 0.00001));
		obstruction += surfaceZ >= samplePosition.z + ao.Parameters.y ? rangeWeight : 0.0;
	}
	outOcclusion = pow(clamp(1.0 - obstruction / 32.0, 0.0, 1.0), ao.Parameters.z);
}
