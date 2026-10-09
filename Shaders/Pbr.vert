#version 450
invariant gl_Position;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec2 aTexCoord1;
layout(location = 5) in vec4 aColor;

struct Light
{
	vec4 PositionType;
	vec4 DirectionRange;
	vec4 ColorIntensity;
	vec4 Cone;
	mat4 ShadowMatrices[6];
};
layout(set = 0, binding = 256, std140) uniform FrameData
{
	mat4 ViewProjection;
	vec4 CameraPosition;
	vec4 Parameters;
	vec4 Environment;
	vec4 Shadow;
	Light Lights[32];
} frame;
layout(push_constant) uniform DrawData
{
	mat4 Model;
	vec4 Color;
	vec4 Material;
} draw;

layout(location = 0) out vec3 vPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vTexCoord;
layout(location = 4) out vec2 vTexCoord1;
layout(location = 5) out vec4 vColor;

void main()
{
	vec4 world = draw.Model * vec4(aPosition, 1.0);
	vPosition = world.xyz;
	vNormal = normalize(transpose(inverse(mat3(draw.Model))) * aNormal);
	vTangent = vec4(normalize(mat3(draw.Model) * aTangent.xyz), aTangent.w * sign(determinant(mat3(draw.Model))));
	vTexCoord = aTexCoord;
	vTexCoord1 = aTexCoord1;
	vColor = aColor * draw.Color;
	gl_Position = frame.ViewProjection * world;
}
