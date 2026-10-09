#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>

namespace Aster
{
	struct TransformComponent
	{
		glm::vec3 Translation{0.0f};
		// Euler angles in radians, composed as Z * Y * X.
		glm::vec3 Rotation{0.0f};
		glm::vec3 Scale{1.0f};
		[[nodiscard]] glm::mat4 GetMatrix() const;
	};

	struct CameraComponent
	{
		float VerticalFov = 60.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;
		bool Primary = true;
	};

	struct MeshRendererComponent
	{
		std::string Mesh;
		glm::vec4 BaseColor{1.0f};
		// Material factors multiply the imported glTF material values.
		float Metallic = 1.0f;
		float Roughness = 1.0f;
		bool Visible = true;
	};

	enum class LightType
	{
		Directional,
		Point,
		Spot
	};

	struct LightComponent
	{
		LightType Type = LightType::Directional;
		glm::vec3 Color{1.0f};
		float Intensity = 1.0f;
		float Range = 10.0f;
		float InnerCone = 20.0f;
		float OuterCone = 30.0f;
		bool CastShadows = true;
	};

	enum class BodyType
	{
		Static,
		Kinematic,
		Dynamic
	};

	enum class CollisionShape
	{
		Box,
		Sphere,
		Capsule
	};

	struct RigidBodyComponent
	{
		BodyType Type = BodyType::Static;
		CollisionShape Shape = CollisionShape::Box;
		glm::vec3 HalfExtents{0.5f};
		glm::vec3 LinearVelocity{0.0f};
		float Radius = 0.5f;
		float Height = 1.0f;
		float Mass = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool IsTrigger = false;
	};

	struct ScriptComponent
	{
		std::string Path;
		bool Enabled = true;
	};

	struct AudioSourceComponent
	{
		std::string Path;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = false;
		bool Spatial = true;
	};

	struct EntityData
	{
		std::string Name = "Entity";
		TransformComponent Transform;
		std::optional<CameraComponent> Camera;
		std::optional<MeshRendererComponent> MeshRenderer;
		std::optional<LightComponent> Light;
		std::optional<RigidBodyComponent> RigidBody;
		std::optional<ScriptComponent> Script;
		std::optional<AudioSourceComponent> AudioSource;
	};
} // namespace Aster
