#version 450
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;
struct Light
{
	vec4 PositionType;
	vec4 DirectionRange;
	vec4 ColorIntensity;
	vec4 Cone;
	mat4 ShadowMatrices[6];
};
layout(set = 1, binding = 256, std140) uniform FrameData
{
	mat4 ViewProjection;
	vec4 CameraPosition;
	vec4 Parameters;
	vec4 Environment;
	vec4 Shadow;
	Light Lights[32];
} frame;
layout(set = 1, binding = 3) uniform texture2D tSky;
layout(set = 1, binding = 128) uniform sampler sEnvironment;
void main()
{
	vec4 world = inverse(frame.ViewProjection) * vec4(vTexCoord.x * 2.0 - 1.0, 1.0 - vTexCoord.y * 2.0, 1.0, 1.0);
	vec3 direction = normalize(world.xyz / world.w - frame.CameraPosition.xyz);
	direction = vec3(frame.Environment.x * direction.x - frame.Environment.y * direction.z, direction.y, frame.Environment.y * direction.x + frame.Environment.x * direction.z);
	vec2 uv = vec2(atan(direction.z, direction.x) / 6.28318530718 + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / 3.14159265359);
	vec3 radiance = texture(sampler2D(tSky, sEnvironment), uv).rgb * frame.Parameters.w;
	outColor = vec4(clamp(radiance, 0.0, 65504.0), 1.0);
}
