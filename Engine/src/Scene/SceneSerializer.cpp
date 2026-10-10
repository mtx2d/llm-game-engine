#include "Aster/Scene/Scene.h"
#include <Aster/Core/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace Aster
{
	namespace
	{
		using Json = nlohmann::json;

		void CheckKeys(const Json& object, std::initializer_list<std::string_view> allowed)
		{
			if (!object.is_object())
			{
				throw std::invalid_argument("Expected a JSON object");
			}
			for (const auto& [key, value] : object.items())
			{
				(void)value;
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
				{
					throw std::invalid_argument("Unknown scene field: " + key);
				}
			}
		}

		uint64_t ReadID(const Json& value)
		{
			if (!value.is_number_integer() ||
				(value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() <= 0))
			{
				throw std::invalid_argument("Entity IDs must be positive integers");
			}
			const auto id = value.get<uint64_t>();
			if (id == 0 || id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				throw std::invalid_argument("Entity ID is outside supported range");
			}
			return id;
		}

		float ReadFloat(const Json& value)
		{
			if (!value.is_number())
			{
				throw std::invalid_argument("Expected a numeric component value");
			}
			const double number = value.get<double>();
			if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
			{
				throw std::invalid_argument("Component value exceeds the finite float range");
			}
			return static_cast<float>(number);
		}

		glm::vec3 ReadVector3(const Json& value)
		{
			if (!value.is_array() || value.size() != 3)
			{
				throw std::invalid_argument("Expected a three-component vector");
			}
			return {ReadFloat(value.at(0)), ReadFloat(value.at(1)), ReadFloat(value.at(2))};
		}

		glm::vec4 ReadVector4(const Json& value)
		{
			if (!value.is_array() || value.size() != 4)
			{
				throw std::invalid_argument("Expected a four-component vector");
			}
			return {ReadFloat(value.at(0)), ReadFloat(value.at(1)), ReadFloat(value.at(2)), ReadFloat(value.at(3))};
		}

		Json WriteVector(const glm::vec3& value)
		{
			return Json::array({value.x, value.y, value.z});
		}

		Json WriteVector(const glm::vec4& value)
		{
			return Json::array({value.x, value.y, value.z, value.w});
		}

		template <typename Enum>
		Enum ReadEnum(const Json& value, std::initializer_list<std::pair<std::string_view, Enum>> values)
		{
			const auto text = value.get<std::string>();
			for (const auto& [name, enumerator] : values)
			{
				if (text == name)
				{
					return enumerator;
				}
			}
			throw std::invalid_argument("Unsupported enum value: " + text);
		}
	} // namespace

	nlohmann::json Scene::Serialize() const
	{
		Validate();
		Json entities = Json::array();
		auto snapshot = Entities();
		std::sort(snapshot.begin(), snapshot.end(),
				  [this](Entity left, Entity right) { return GetPersistentID(left) < GetPersistentID(right); });
		for (const auto entity : snapshot)
		{
			const auto& data = Get(entity);
			const auto parent = GetParent(entity);
			Json serialized = {{"ID", GetPersistentID(entity)},
							   {"Name", data.Name},
							   {"Parent", parent ? Json(GetPersistentID(parent)) : Json(nullptr)},
							   {"Transform",
								{{"Translation", WriteVector(data.Transform.Translation)},
								 {"Rotation", WriteVector(data.Transform.Rotation)},
								 {"Scale", WriteVector(data.Transform.Scale)}}}};
			if (data.Camera)
			{
				const auto& camera = *data.Camera;
				serialized["Camera"] = {{"VerticalFov", camera.VerticalFov},
										{"NearClip", camera.NearClip},
										{"FarClip", camera.FarClip},
										{"Primary", camera.Primary}};
			}
			if (data.MeshRenderer)
			{
				const auto& mesh = *data.MeshRenderer;
				serialized["MeshRenderer"] = {{"Mesh", mesh.Mesh},
											  {"BaseColor", WriteVector(mesh.BaseColor)},
											  {"Metallic", mesh.Metallic},
											  {"Roughness", mesh.Roughness},
											  {"Visible", mesh.Visible}};
			}
			if (data.Light)
			{
				const auto& light = *data.Light;
				const char* type = light.Type == LightType::Directional ? "Directional"
								   : light.Type == LightType::Point		? "Point"
																		: "Spot";
				serialized["Light"] = {{"Type", type},
									   {"Color", WriteVector(light.Color)},
									   {"Intensity", light.Intensity},
									   {"Range", light.Range},
									   {"InnerCone", light.InnerCone},
									   {"OuterCone", light.OuterCone},
									   {"CastShadows", light.CastShadows}};
			}
			if (data.RigidBody)
			{
				const auto& body = *data.RigidBody;
				const char* type = body.Type == BodyType::Static	  ? "Static"
								   : body.Type == BodyType::Kinematic ? "Kinematic"
																	  : "Dynamic";
				const char* shape = body.Shape == CollisionShape::Box	   ? "Box"
									: body.Shape == CollisionShape::Sphere ? "Sphere"
																		   : "Capsule";
				serialized["RigidBody"] = {{"Type", type},
										   {"Shape", shape},
										   {"HalfExtents", WriteVector(body.HalfExtents)},
										   {"LinearVelocity", WriteVector(body.LinearVelocity)},
										   {"Radius", body.Radius},
										   {"Height", body.Height},
										   {"Mass", body.Mass},
										   {"Friction", body.Friction},
										   {"Restitution", body.Restitution},
										   {"IsTrigger", body.IsTrigger}};
			}
			if (data.Script)
			{
				serialized["Script"] = {{"Path", data.Script->Path}, {"Enabled", data.Script->Enabled}};
			}
			if (data.AudioSource)
			{
				const auto& audio = *data.AudioSource;
				serialized["AudioSource"] = {
					{"Path", audio.Path}, {"Volume", audio.Volume},			  {"Pitch", audio.Pitch},
					{"Loop", audio.Loop}, {"PlayOnStart", audio.PlayOnStart}, {"Spatial", audio.Spatial}};
			}
			entities.push_back(std::move(serialized));
		}
		return {{"Version", 1},
				{"Name", m_Name},
				{"NextEntityID", m_NextPersistentID},
				{"Environment",
				 {{"Path", m_Environment.Path},
				  {"Intensity", m_Environment.Intensity},
				  {"Rotation", m_Environment.Rotation}}},
				{"Entities", std::move(entities)}};
	}

	Scene Scene::Deserialize(const nlohmann::json& document)
	{
		CheckKeys(document, {"Version", "Name", "NextEntityID", "Environment", "Entities"});
		if (!document.at("Version").is_number_integer() || document.at("Version") != 1)
		{
			throw std::invalid_argument("Unsupported scene version");
		}
		const auto& entities = document.at("Entities");
		if (!entities.is_array() || entities.size() > 100000)
		{
			throw std::invalid_argument("Scene must contain an entity array with at most 100000 entries");
		}
		Scene scene(document.at("Name").get<std::string>());
		if (document.contains("Environment"))
		{
			const auto& environment = document.at("Environment");
			CheckKeys(environment, {"Path", "Intensity", "Rotation"});
			scene.SetEnvironment({environment.at("Path").get<std::string>(), ReadFloat(environment.at("Intensity")),
								  ReadFloat(environment.at("Rotation"))});
		}
		for (const auto& serialized : entities)
		{
			CheckKeys(serialized, {"ID", "Name", "Parent", "Transform", "Camera", "MeshRenderer", "Light", "RigidBody",
								   "Script", "AudioSource"});
			const auto entity =
				scene.CreateWithID(serialized.at("Name").get<std::string>(), ReadID(serialized.at("ID")));
			auto& data = scene.Get(entity);
			const auto& transform = serialized.at("Transform");
			CheckKeys(transform, {"Translation", "Rotation", "Scale"});
			data.Transform = {ReadVector3(transform.at("Translation")), ReadVector3(transform.at("Rotation")),
							  ReadVector3(transform.at("Scale"))};
			if (serialized.contains("Camera"))
			{
				const auto& source = serialized.at("Camera");
				CheckKeys(source, {"VerticalFov", "NearClip", "FarClip", "Primary"});
				data.Camera = CameraComponent{ReadFloat(source.at("VerticalFov")), ReadFloat(source.at("NearClip")),
											  ReadFloat(source.at("FarClip")), source.at("Primary").get<bool>()};
			}
			if (serialized.contains("MeshRenderer"))
			{
				const auto& source = serialized.at("MeshRenderer");
				CheckKeys(source, {"Mesh", "BaseColor", "Metallic", "Roughness", "Visible"});
				data.MeshRenderer =
					MeshRendererComponent{source.at("Mesh").get<std::string>(), ReadVector4(source.at("BaseColor")),
										  ReadFloat(source.at("Metallic")), ReadFloat(source.at("Roughness")),
										  source.at("Visible").get<bool>()};
			}
			if (serialized.contains("Light"))
			{
				const auto& source = serialized.at("Light");
				CheckKeys(source, {"Type", "Color", "Intensity", "Range", "InnerCone", "OuterCone", "CastShadows"});
				data.Light =
					LightComponent{ReadEnum<LightType>(source.at("Type"), {{"Directional", LightType::Directional},
																		   {"Point", LightType::Point},
																		   {"Spot", LightType::Spot}}),
								   ReadVector3(source.at("Color")),
								   ReadFloat(source.at("Intensity")),
								   ReadFloat(source.at("Range")),
								   ReadFloat(source.at("InnerCone")),
								   ReadFloat(source.at("OuterCone")),
								   source.at("CastShadows").get<bool>()};
			}
			if (serialized.contains("RigidBody"))
			{
				const auto& source = serialized.at("RigidBody");
				CheckKeys(source, {"Type", "Shape", "HalfExtents", "LinearVelocity", "Radius", "Height", "Mass",
								   "Friction", "Restitution", "IsTrigger"});
				data.RigidBody = RigidBodyComponent{
					ReadEnum<BodyType>(source.at("Type"), {{"Static", BodyType::Static},
														   {"Kinematic", BodyType::Kinematic},
														   {"Dynamic", BodyType::Dynamic}}),
					ReadEnum<CollisionShape>(source.at("Shape"), {{"Box", CollisionShape::Box},
																  {"Sphere", CollisionShape::Sphere},
																  {"Capsule", CollisionShape::Capsule}}),
					ReadVector3(source.at("HalfExtents")),
					ReadVector3(source.at("LinearVelocity")),
					ReadFloat(source.at("Radius")),
					ReadFloat(source.at("Height")),
					ReadFloat(source.at("Mass")),
					ReadFloat(source.at("Friction")),
					ReadFloat(source.at("Restitution")),
					source.at("IsTrigger").get<bool>()};
			}
			if (serialized.contains("Script"))
			{
				const auto& source = serialized.at("Script");
				CheckKeys(source, {"Path", "Enabled"});
				data.Script = ScriptComponent{source.at("Path").get<std::string>(), source.at("Enabled").get<bool>()};
			}
			if (serialized.contains("AudioSource"))
			{
				const auto& source = serialized.at("AudioSource");
				CheckKeys(source, {"Path", "Volume", "Pitch", "Loop", "PlayOnStart", "Spatial"});
				data.AudioSource =
					AudioSourceComponent{source.at("Path").get<std::string>(), ReadFloat(source.at("Volume")),
										 ReadFloat(source.at("Pitch")),		   source.at("Loop").get<bool>(),
										 source.at("PlayOnStart").get<bool>(), source.at("Spatial").get<bool>()};
			}
		}
		// Two passes allow parents to appear after children in a document.
		for (const auto& serialized : entities)
		{
			const auto& parent = serialized.at("Parent");
			if (!parent.is_null())
			{
				const auto parentEntity = scene.FindByID(ReadID(parent));
				if (!parentEntity)
				{
					throw std::invalid_argument("Entity references a missing parent");
				}
				scene.GetSlot(scene.FindByID(ReadID(serialized.at("ID")))).Parent = parentEntity;
			}
		}
		if (document.contains("NextEntityID"))
		{
			const auto& nextID = document.at("NextEntityID");
			if (!nextID.is_number_integer() || (!nextID.is_number_unsigned() && nextID.get<int64_t>() <= 0))
			{
				throw std::invalid_argument("NextEntityID must be a positive integer");
			}
			const auto value = nextID.get<uint64_t>();
			const auto exhaustedID = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
			if (value < scene.m_NextPersistentID || value > exhaustedID)
			{
				throw std::invalid_argument("NextEntityID would reuse an existing ID or exceeds the supported range");
			}
			scene.m_NextPersistentID = value;
		}
		scene.Validate();
		return scene;
	}

	void Scene::ReplaceFromJson(const nlohmann::json& document)
	{
		auto replacement = Deserialize(document);
		Swap(replacement);
	}

	void Scene::Save(const std::filesystem::path& path) const
	{
		const auto contents = Serialize().dump(2);
		if (contents.size() > 64ULL * 1024ULL * 1024ULL)
		{
			throw std::invalid_argument("Scene exceeds the 64 MiB document size limit");
		}
		WriteTextFileAtomically(path, contents);
	}

	Scene Scene::Load(const std::filesystem::path& path)
	{
		return Deserialize(ReadJsonFile(path, 64ULL * 1024ULL * 1024ULL));
	}
} // namespace Aster
