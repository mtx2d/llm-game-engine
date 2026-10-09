#version 450

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vTexCoord;
layout(location = 4) in vec2 vTexCoord1;
layout(location = 5) in vec4 vColor;
layout(location = 0) out vec4 outColor;

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
layout(set = 0, binding = 257, std140) uniform MaterialData
{
	vec4 BaseColor;
	vec4 EmissiveMetallic;
	vec4 Parameters;
	vec4 Transforms[15];
	vec4 TextureInfo[5];
} material;
layout(push_constant) uniform DrawData
{
	mat4 Model;
	vec4 Color;
	vec4 Material;
} draw;
layout(set = 0, binding = 0) uniform texture2D tBaseColor;
layout(set = 0, binding = 1) uniform texture2D tMetallicRoughness;
layout(set = 0, binding = 2) uniform texture2D tNormal;
layout(set = 0, binding = 3) uniform texture2D tOcclusion;
layout(set = 0, binding = 4) uniform texture2D tEmissive;
layout(set = 0, binding = 128) uniform sampler sBaseColor;
layout(set = 0, binding = 129) uniform sampler sMetallicRoughness;
layout(set = 0, binding = 130) uniform sampler sNormal;
layout(set = 0, binding = 131) uniform sampler sOcclusion;
layout(set = 0, binding = 132) uniform sampler sEmissive;
layout(set = 1, binding = 0) uniform texture2D tIrradiance;
layout(set = 1, binding = 1) uniform texture2D tSpecular;
layout(set = 1, binding = 2) uniform texture2D tBrdf;
layout(set = 1, binding = 4) uniform texture2DArray tShadows;
layout(set = 1, binding = 5) uniform texture2D tAmbientOcclusion;
layout(set = 1, binding = 128) uniform sampler sEnvironment;
layout(set = 1, binding = 129) uniform sampler sBrdf;
layout(set = 1, binding = 130) uniform sampler sShadow;

const float Pi = 3.14159265359;

vec3 SafeNormalize(vec3 direction)
{
	float lengthSquared = dot(direction, direction);
	return lengthSquared > 0.00000001 ? direction * inversesqrt(lengthSquared) : vec3(0.0, 0.0, 1.0);
}

vec2 EnvironmentCoordinate(vec3 direction)
{
	direction = vec3(frame.Environment.x * direction.x - frame.Environment.y * direction.z, direction.y, frame.Environment.y * direction.x + frame.Environment.x * direction.z);
	return vec2(atan(direction.z, direction.x) / (2.0 * Pi) + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / Pi);
}

vec2 TextureCoordinate(int index)
{
	vec2 uv = material.TextureInfo[index].x < 0.5 ? vTexCoord : vTexCoord1;
	mat3 transform = mat3(material.Transforms[index * 3].xyz, material.Transforms[index * 3 + 1].xyz, material.Transforms[index * 3 + 2].xyz);
	return (transform * vec3(uv, 1.0)).xy;
}

vec3 Fresnel(float cosine, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

float Distribution(float nDotH, float roughness)
{
	float alpha = roughness * roughness;
	float alphaSquared = alpha * alpha;
	float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(Pi * denominator * denominator, 0.000001);
}

float Visibility(float nDotV, float nDotL, float roughness)
{
	float alpha = roughness * roughness;
	float alphaSquared = alpha * alpha;
	float view = nDotL * sqrt(max(nDotV * nDotV * (1.0 - alphaSquared) + alphaSquared, 0.000001));
	float light = nDotV * sqrt(max(nDotL * nDotL * (1.0 - alphaSquared) + alphaSquared, 0.000001));
	return 0.5 / max(view + light, 0.000001);
}

float ShadowVisibility(Light light, vec3 normal, vec3 lightDirection, vec3 positionDx, vec3 positionDy)
{
	if (light.Cone.z < 0.0) { return 1.0; }
	int face = 0;
	if (light.PositionType.w > 0.5 && light.PositionType.w < 1.5)
	{
		vec3 direction = vPosition - light.PositionType.xyz;
		vec3 absoluteDirection = abs(direction);
		if (absoluteDirection.x >= absoluteDirection.y && absoluteDirection.x >= absoluteDirection.z)
		{
			face = direction.x >= 0.0 ? 0 : 1;
		}
		else if (absoluteDirection.y >= absoluteDirection.z)
		{
			face = direction.y >= 0.0 ? 2 : 3;
		}
		else { face = direction.z >= 0.0 ? 4 : 5; }
	}
	vec4 projected = light.ShadowMatrices[face] * vec4(vPosition, 1.0);
	vec3 position = projected.xyz / projected.w;
	vec2 uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
	if (position.z <= 0.0 || position.z >= 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
	{
		return 1.0;
	}
	float bias = frame.Shadow.x * (1.0 + 2.0 * (1.0 - max(dot(normal, lightDirection), 0.0)));
	if (light.PositionType.w > 0.5) { bias *= max(1.0 - position.z, 0.00001); }
	// A sloped receiver has a different depth at every PCF tap. Transform its
	// geometric plane through the selected projection, including perspective W,
	// so nearby receiver texels are not mistaken for occluders.
	vec4 projectedDx = light.ShadowMatrices[face] * vec4(positionDx, 0.0);
	vec4 projectedDy = light.ShadowMatrices[face] * vec4(positionDy, 0.0);
	vec3 depthDx = (projectedDx.xyz - position * projectedDx.w) / projected.w;
	vec3 depthDy = (projectedDy.xyz - position * projectedDy.w) / projected.w;
	vec3 receiverPlane = cross(depthDx * vec3(0.5, -0.5, 1.0), depthDy * vec3(0.5, -0.5, 1.0));
	vec2 depthGradient = vec2(0.0);
	if (abs(receiverPlane.z) > 0.00001 * length(receiverPlane))
	{
		depthGradient = -receiverPlane.xy / receiverPlane.z;
	}
	ivec2 dimensions = textureSize(sampler2DArray(tShadows, sShadow), 0).xy;
	vec2 texel = frame.Shadow.y / vec2(dimensions);
	float visibility = 0.0;
	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			ivec2 sampleTexel = clamp(ivec2(floor((uv + vec2(x, y) * texel) * vec2(dimensions))), ivec2(0), dimensions - 1);
			vec2 sampleUv = (vec2(sampleTexel) + 0.5) / vec2(dimensions);
			float receiverDepth = position.z + dot(depthGradient, sampleUv - uv);
			float depth = texelFetch(sampler2DArray(tShadows, sShadow), ivec3(sampleTexel, int(light.Cone.z) + face), 0).r;
			visibility += receiverDepth - bias <= depth ? 1.0 : 0.0;
		}
	}
	return visibility / 25.0;
}

void main()
{
	// Derivatives must be evaluated before material discards and divergent
	// light/point-face branches; the plane uses geometry, not normal-map detail.
	vec3 positionDx = dFdx(vPosition);
	vec3 positionDy = dFdy(vPosition);
	vec4 baseColor = material.BaseColor * vColor * texture(sampler2D(tBaseColor, sBaseColor), TextureCoordinate(0));
	if (material.Parameters.z > 0.5 && material.Parameters.z < 1.5 && baseColor.a < material.Parameters.y)
	{
		discard;
	}
	float alpha = material.Parameters.z > 1.5 ? baseColor.a : 1.0;
	vec3 emission = material.EmissiveMetallic.rgb * texture(sampler2D(tEmissive, sEmissive), TextureCoordinate(4)).rgb;
	if (material.Parameters.w > 0.5)
	{
		outColor = vec4(clamp(baseColor.rgb + emission, 0.0, 65504.0), alpha);
		return;
	}
	vec4 metallicRoughness = texture(sampler2D(tMetallicRoughness, sMetallicRoughness), TextureCoordinate(1));
	float metallic = clamp(material.EmissiveMetallic.w * draw.Material.x * metallicRoughness.b, 0.0, 1.0);
	float roughness = clamp(material.Parameters.x * draw.Material.y * metallicRoughness.g, 0.045, 1.0);
	vec3 normal = SafeNormalize(vNormal);
	if (!gl_FrontFacing)
	{
		normal = -normal;
	}
	if (material.TextureInfo[2].z > 0.5)
	{
		vec3 tangent = vTangent.xyz - normal * dot(normal, vTangent.xyz);
		if (dot(tangent, tangent) < 0.00000001)
		{
			tangent = cross(abs(normal.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), normal);
		}
		tangent = SafeNormalize(tangent);
		vec3 bitangent = cross(normal, tangent) * vTangent.w;
		vec3 sampledNormal = texture(sampler2D(tNormal, sNormal), TextureCoordinate(2)).xyz * 2.0 - 1.0;
		sampledNormal.xy *= material.TextureInfo[2].y;
		normal = SafeNormalize(mat3(tangent, bitangent, normal) * sampledNormal);
	}
	vec3 viewDirection = SafeNormalize(frame.CameraPosition.xyz - vPosition);
	float nDotV = max(dot(normal, viewDirection), 0.0001);
	vec3 f0 = mix(vec3(0.04), baseColor.rgb, metallic);
	float occlusion = mix(1.0, texture(sampler2D(tOcclusion, sOcclusion), TextureCoordinate(3)).r, material.TextureInfo[3].y);
	occlusion *= material.Parameters.z > 1.5 ? 1.0 : texelFetch(sampler2D(tAmbientOcclusion, sShadow), ivec2(gl_FragCoord.xy), 0).r;
	vec3 color = baseColor.rgb * (1.0 - metallic) * frame.Parameters.z * occlusion + emission;
	if (frame.Parameters.w > 0.0)
	{
		vec3 irradiance = texture(sampler2D(tIrradiance, sEnvironment), EnvironmentCoordinate(normal)).rgb;
		vec3 reflected = reflect(-viewDirection, normal);
		float lod = roughness * float(textureQueryLevels(sampler2D(tSpecular, sEnvironment)) - 1);
		vec3 prefiltered = textureLod(sampler2D(tSpecular, sEnvironment), EnvironmentCoordinate(reflected), lod).rgb;
		vec2 brdf = texture(sampler2D(tBrdf, sBrdf), vec2(nDotV, roughness)).rg;
		vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nDotV, 5.0);
		vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor.rgb * irradiance / Pi;
		color += (diffuse + prefiltered * (f0 * brdf.x + brdf.y)) * frame.Parameters.w * occlusion;
	}
	for (int index = 0; index < int(frame.Parameters.x); ++index)
	{
		Light light = frame.Lights[index];
		vec3 lightDirection;
		float attenuation = 1.0;
		if (light.PositionType.w < 0.5)
		{
			lightDirection = -light.DirectionRange.xyz;
		}
		else
		{
			vec3 toLight = light.PositionType.xyz - vPosition;
			float distanceSquared = max(dot(toLight, toLight), 0.0001);
			float distanceToLight = sqrt(distanceSquared);
			lightDirection = toLight / distanceToLight;
			float rangeFalloff = clamp(1.0 - pow(distanceToLight / light.DirectionRange.w, 4.0), 0.0, 1.0);
			attenuation = rangeFalloff * rangeFalloff / distanceSquared;
			if (light.PositionType.w > 1.5)
			{
				float cosine = dot(light.DirectionRange.xyz, -lightDirection);
				float cone = clamp((cosine - light.Cone.y) / max(light.Cone.x - light.Cone.y, 0.00001), 0.0, 1.0);
				attenuation *= cone * cone * (3.0 - 2.0 * cone);
			}
		}
		float nDotL = max(dot(normal, lightDirection), 0.0);
		vec3 halfDirection = SafeNormalize(viewDirection + lightDirection);
		float nDotH = max(dot(normal, halfDirection), 0.0);
		float vDotH = max(dot(viewDirection, halfDirection), 0.0);
		vec3 fresnel = Fresnel(vDotH, f0);
		vec3 specular = Distribution(nDotH, roughness) * Visibility(nDotV, nDotL, roughness) * fresnel;
		vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor.rgb / Pi;
		float shadow = nDotL > 0.0 && attenuation > 0.0 ? ShadowVisibility(light, normal, lightDirection, positionDx, positionDy) : 1.0;
		color += (diffuse + specular) * light.ColorIntensity.rgb * light.ColorIntensity.w * attenuation * nDotL * shadow;
	}
	outColor = vec4(clamp(color, 0.0, 65504.0), alpha);
}
