#include "Aster/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>

namespace
{
	using namespace Aster;
	using Json = nlohmann::json;

	void Check(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error("Scene test failed: " + message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const std::string& message)
	{
		bool rejected = false;
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Check(rejected, message);
	}

	void TestEntityLifetime()
	{
		Scene scene("Lifetime");
		const auto first = scene.CreateEntity("First");
		const auto firstID = scene.GetPersistentID(first);
		Check(scene.IsAlive(first) && scene.Size() == 1, "created entity is alive");
		Scene foreign;
		const auto foreignEntity = foreign.CreateEntity();
		Check(!foreign.IsAlive(first) && !scene.IsAlive(foreignEntity), "cross-scene handles rejected");
		Rejects([&] { (void)scene.Get(foreignEntity); }, "cross-scene access throws");
		Rejects([&] { scene.SetParent(first, foreignEntity); }, "cross-scene parenting throws");
		scene.DestroyEntity(first);
		Check(!scene.IsAlive(first) && !scene.FindByID(firstID), "destroyed identity disappears");
		Rejects([&] { (void)scene.Get(first); }, "stale reads throw");
		Rejects([&] { scene.DestroyEntity(first); }, "double destroy throws");
		const auto replacement = scene.CreateEntity("Replacement");
		Check(replacement.Index == first.Index && replacement.Generation != first.Generation,
			  "slot generation increments");
		Check(!scene.IsAlive(first) && scene.GetPersistentID(replacement) != firstID,
			  "slot reuse cannot resurrect an ID");
		const auto snapshot = scene.Entities();
		for (const auto entity : snapshot)
		{
			scene.DestroyEntity(entity);
			(void)scene.CreateEntity("Created during iteration");
		}
		Check(snapshot.size() == 1 && scene.Size() == 1, "snapshot iteration tolerates structural changes");
		const auto survivor = scene.Entities().front();
		auto* stableData = &scene.Get(survivor);
		for (int index = 0; index < 2000; ++index)
		{
			(void)scene.CreateEntity();
		}
		Check(&scene.Get(survivor) == stableData, "component record address survives slot growth");
		Scene moved(std::move(scene));
		Check(moved.IsAlive(survivor) && !scene.IsAlive(survivor), "move transfers handle ownership");
		scene.SetName("Reused moved scene");
		Check(scene.IsAlive(scene.CreateEntity()), "moved-from scene can be reused");
		const auto invalidated = foreignEntity;
		foreign = std::move(moved);
		Check(foreign.IsAlive(survivor) && !foreign.IsAlive(invalidated),
			  "move assignment invalidates destination handles");
	}

	void TestHierarchyAndPrefabs()
	{
		Scene scene("Hierarchy");
		const auto root = scene.CreateEntity("Root");
		const auto child = scene.CreateEntity("Child");
		const auto grandchild = scene.CreateEntity("Grandchild");
		scene.Get(root).Transform.Translation = {3.0f, 0.0f, 0.0f};
		scene.Get(root).Transform.Scale = glm::vec3(2.0f);
		scene.Get(child).Transform.Translation = {0.0f, 4.0f, 0.0f};
		scene.Get(grandchild).Transform.Translation = {0.0f, 0.0f, 5.0f};
		scene.SetParent(child, root);
		scene.SetParent(grandchild, child);
		const auto position = glm::vec3(scene.GetWorldTransform(grandchild)[3]);
		Check(glm::length(position - glm::vec3(3.0f, 8.0f, 10.0f)) < 0.0001f, "world matrix composes ancestry");
		Rejects([&] { scene.SetParent(root, grandchild); }, "ancestor cycle rejected");
		Rejects([&] { scene.SetParent(root, root); }, "self cycle rejected");
		Check(!scene.GetParent(root), "failed reparent leaves hierarchy unchanged");
		scene.SetParent(grandchild);
		Check(!scene.GetParent(grandchild), "entity can detach");
		scene.SetParent(grandchild, child);
		scene.Get(child).Script = ScriptComponent{"Scripts/Child.lua", true};
		Scene target("Prefab destination");
		const auto external = target.CreateEntity("External parent");
		const auto clone = target.InstantiatePrefab(scene, root, external);
		Check(target.Size() == 4 && target.GetParent(clone) == external, "prefab root attaches to requested parent");
		Entity clonedChild;
		Entity clonedGrandchild;
		for (const auto entity : target.Entities())
		{
			if (target.Get(entity).Name == "Child")
			{
				clonedChild = entity;
			}
			if (target.Get(entity).Name == "Grandchild")
			{
				clonedGrandchild = entity;
			}
		}
		Check(target.GetParent(clonedChild) == clone && target.GetParent(clonedGrandchild) == clonedChild,
			  "prefab internal parents remapped to cloned entities");
		Check(target.Get(clonedChild).Script->Path == "Scripts/Child.lua", "prefab components copied");
		target.Get(clonedChild).Transform.Translation.x = 99.0f;
		Check(scene.Get(child).Transform.Translation.x == 0.0f, "prefab edits do not mutate source");
		const auto selfClone = scene.InstantiatePrefab(scene, child);
		Check(scene.Size() == 5 && !scene.GetParent(selfClone), "same-scene subtree cloning succeeds");
		scene.DestroyEntity(root);
		Check(scene.Size() == 2 && scene.IsAlive(selfClone) && !scene.IsAlive(child) && !scene.IsAlive(grandchild),
			  "recursive destruction affects only original subtree");
		target.DestroyEntity(clone);
		Check(target.Size() == 1 && target.IsAlive(external), "destroying clone preserves external parent");
		TransformComponent rotation;
		rotation.Rotation.z = 1.57079632679f;
		const auto rotated = glm::vec3(rotation.GetMatrix() * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
		Check(glm::length(rotated - glm::vec3(0.0f, 1.0f, 0.0f)) < 0.0001f, "rotations use radians");
	}

	Scene MakeComponentScene()
	{
		Scene scene("All components");
		scene.SetEnvironment({"Environment/StudioSmall09.hdr", 1.25f, 0.5f});
		const auto entity = scene.CreateEntity("Featured entity");
		auto& data = scene.Get(entity);
		data.Transform = {{1.0f, 2.0f, 3.0f}, {0.2f, -0.1f, 0.5f}, {-2.0f, 3.0f, 4.0f}};
		data.Camera = CameraComponent{75.0f, 0.25f, 500.0f, false};
		data.MeshRenderer = MeshRendererComponent{"Models/Test.glb", {0.2f, 0.3f, 0.4f, 0.5f}, 0.8f, 0.2f, false};
		data.Light = LightComponent{LightType::Spot, {0.5f, 0.7f, 1.0f}, 4.0f, 23.0f, 17.0f, 32.0f, false};
		data.RigidBody = RigidBodyComponent{BodyType::Dynamic,
											CollisionShape::Capsule,
											{1.0f, 2.0f, 3.0f},
											{2.0f, 3.0f, 4.0f},
											0.8f,
											2.0f,
											3.0f,
											0.7f,
											0.3f,
											true};
		data.Script = ScriptComponent{"Scripts/Test.lua", false};
		data.AudioSource = AudioSourceComponent{"Audio/Test.wav", 0.4f, 1.5f, true, true, false};
		const auto child = scene.CreateEntity("Child");
		scene.SetParent(child, entity);
		return scene;
	}

	void TestSerializationAndInput()
	{
		auto scene = MakeComponentScene();
		const auto original = scene.Serialize();
		auto loaded = Scene::Deserialize(Json::parse(original.dump()));
		Check(loaded.Serialize() == original, "every component field round trips");
		Check(!loaded.IsAlive(scene.Entities().front()), "deserialized scene rejects old handles");
		Check(loaded.GetEnvironment().Path == "Environment/StudioSmall09.hdr" &&
				  loaded.GetEnvironment().Intensity == 1.25f && loaded.GetEnvironment().Rotation == 0.5f,
			  "environment selection and controls round trip");
		auto withoutEnvironment = original;
		withoutEnvironment.erase("Environment");
		Check(Scene::Deserialize(withoutEnvironment).GetEnvironment().Path.empty(),
			  "legacy scene environment defaults to disabled");
		const auto environmentBefore = scene.Serialize();
		Rejects([&] { scene.SetEnvironment({"../Outside.hdr", 1.0f, 0.0f}); }, "environment traversal setter rejected");
		Check(scene.Serialize() == environmentBefore, "invalid environment assignment leaves state unchanged");
		const auto fresh = loaded.CreateEntity();
		Check(loaded.GetPersistentID(fresh) == 3, "loaded ID allocation advances past persisted IDs");
		loaded.DestroyEntity(fresh);
		loaded = Scene::Deserialize(loaded.Serialize());
		Check(loaded.GetPersistentID(loaded.CreateEntity()) == 4,
			  "deleted persistent IDs are never reused after reload");
		auto reversed = original;
		std::reverse(reversed["Entities"].begin(), reversed["Entities"].end());
		Check(Scene::Deserialize(reversed).Serialize() == original,
			  "parent references resolve independently of document order");

		const std::vector<std::pair<std::string, std::function<void(Json&)>>> invalidCases = {
			{"future schema", [](Json& doc) { doc["Version"] = 2; }},
			{"float schema", [](Json& doc) { doc["Version"] = 1.0; }},
			{"missing schema", [](Json& doc) { doc.erase("Version"); }},
			{"reused next ID", [](Json& doc) { doc["NextEntityID"] = 1; }},
			{"negative next ID", [](Json& doc) { doc["NextEntityID"] = -1; }},
			{"fractional next ID", [](Json& doc) { doc["NextEntityID"] = 3.5; }},
			{"oversized next ID", [](Json& doc) { doc["NextEntityID"] = std::numeric_limits<uint64_t>::max(); }},
			{"unknown root field", [](Json& doc) { doc["Typo"] = true; }},
			{"empty scene name", [](Json& doc) { doc["Name"] = ""; }},
			{"environment traversal", [](Json& doc) { doc["Environment"]["Path"] = "../Outside.hdr"; }},
			{"environment absolute path", [](Json& doc) { doc["Environment"]["Path"] = "/Outside.hdr"; }},
			{"negative environment intensity", [](Json& doc) { doc["Environment"]["Intensity"] = -1.0f; }},
			{"boolean environment intensity", [](Json& doc) { doc["Environment"]["Intensity"] = true; }},
			{"nonfinite environment rotation",
			 [](Json& doc) { doc["Environment"]["Rotation"] = std::numeric_limits<double>::infinity(); }},
			{"missing environment field", [](Json& doc) { doc["Environment"].erase("Rotation"); }},
			{"unknown environment field", [](Json& doc) { doc["Environment"]["Typo"] = 1; }},
			{"non-array entities", [](Json& doc) { doc["Entities"] = Json::object(); }},
			{"duplicate entity ID", [](Json& doc) { doc["Entities"][1]["ID"] = 1; }},
			{"zero ID", [](Json& doc) { doc["Entities"][0]["ID"] = 0; }},
			{"negative ID", [](Json& doc) { doc["Entities"][0]["ID"] = -1; }},
			{"float ID", [](Json& doc) { doc["Entities"][0]["ID"] = 1.5; }},
			{"oversized ID", [](Json& doc) { doc["Entities"][0]["ID"] = std::numeric_limits<uint64_t>::max(); }},
			{"missing parent", [](Json& doc) { doc["Entities"][1]["Parent"] = 500; }},
			{"self parent", [](Json& doc) { doc["Entities"][0]["Parent"] = 1; }},
			{"cyclic parent", [](Json& doc) { doc["Entities"][0]["Parent"] = 2; }},
			{"zero parent", [](Json& doc) { doc["Entities"][0]["Parent"] = 0; }},
			{"unknown component", [](Json& doc) { doc["Entities"][0]["ParticleTypo"] = Json::object(); }},
			{"missing transform", [](Json& doc) { doc["Entities"][0].erase("Transform"); }},
			{"short vector", [](Json& doc) { doc["Entities"][0]["Transform"]["Translation"] = {0, 1}; }},
			{"string vector value", [](Json& doc) { doc["Entities"][0]["Transform"]["Translation"][0] = "nan"; }},
			{"boolean vector value", [](Json& doc) { doc["Entities"][0]["Transform"]["Translation"][0] = true; }},
			{"nonfinite position", [](Json& doc)
			 { doc["Entities"][0]["Transform"]["Translation"][0] = std::numeric_limits<double>::infinity(); }},
			{"overflow position", [](Json& doc) { doc["Entities"][0]["Transform"]["Translation"][0] = 1e100; }},
			{"singular transform", [](Json& doc) { doc["Entities"][0]["Transform"]["Scale"][0] = 0; }},
			{"camera range", [](Json& doc) { doc["Entities"][0]["Camera"]["FarClip"] = 0.01; }},
			{"camera field of view", [](Json& doc) { doc["Entities"][0]["Camera"]["VerticalFov"] = 180; }},
			{"mesh escaping path", [](Json& doc) { doc["Entities"][0]["MeshRenderer"]["Mesh"] = "../secret.glb"; }},
			{"invalid material factor", [](Json& doc) { doc["Entities"][0]["MeshRenderer"]["Metallic"] = 2; }},
			{"invalid material alpha", [](Json& doc) { doc["Entities"][0]["MeshRenderer"]["BaseColor"][3] = -1; }},
			{"unknown light", [](Json& doc) { doc["Entities"][0]["Light"]["Type"] = "Unknown"; }},
			{"negative light", [](Json& doc) { doc["Entities"][0]["Light"]["Intensity"] = -1; }},
			{"light cone order", [](Json& doc) { doc["Entities"][0]["Light"]["InnerCone"] = 50; }},
			{"negative collider", [](Json& doc) { doc["Entities"][0]["RigidBody"]["HalfExtents"][1] = -1; }},
			{"massless dynamic body", [](Json& doc) { doc["Entities"][0]["RigidBody"]["Mass"] = 0; }},
			{"unknown shape", [](Json& doc) { doc["Entities"][0]["RigidBody"]["Shape"] = "Triangle"; }},
			{"invalid restitution", [](Json& doc) { doc["Entities"][0]["RigidBody"]["Restitution"] = 1.5; }},
			{"absolute script", [](Json& doc) { doc["Entities"][0]["Script"]["Path"] = "/tmp/script.lua"; }},
			{"wrong script boolean", [](Json& doc) { doc["Entities"][0]["Script"]["Enabled"] = 1; }},
			{"audio pitch", [](Json& doc) { doc["Entities"][0]["AudioSource"]["Pitch"] = 0; }},
			{"audio volume", [](Json& doc) { doc["Entities"][0]["AudioSource"]["Volume"] = -1; }}};
		for (const auto& [name, mutate] : invalidCases)
		{
			auto invalid = original;
			mutate(invalid);
			const auto previousHandle = scene.Entities().front();
			Rejects([&] { scene.ReplaceFromJson(invalid); }, name);
			Check(scene.Serialize() == original && scene.IsAlive(previousHandle),
				  "invalid replacement is atomic: " + name);
		}
		const auto previousHandle = scene.Entities().front();
		scene.ReplaceFromJson(original);
		Check(!scene.IsAlive(previousHandle) && scene.Serialize() == original,
			  "successful replacement invalidates old handles");

		for (const auto& path :
			 {"../asset", "A/../../asset", "A\\..\\asset", "/absolute", "C:\\Asset.glb", "//server/asset", "", "."})
		{
			Rejects([&] { Scene::ValidateAssetPath(path); }, "escaping or invalid asset path rejected");
		}
		Scene::ValidateAssetPath("Meshes/Valid model.glb");
		Rejects([&] { Scene::ValidateAssetPath(std::string("a\0b", 3)); }, "embedded null path rejected");
		auto& data = scene.Get(scene.Entities().front());
		data.Transform.Rotation.x = std::numeric_limits<float>::quiet_NaN();
		Rejects([&] { scene.Validate(); }, "direct invalid component edit rejected");
		Rejects([&] { (void)scene.Serialize(); }, "invalid direct edit cannot be persisted");
	}

	void TestFiles()
	{
		std::random_device random;
		const auto directory = std::filesystem::current_path() / ("SceneTestArtifacts-" + std::to_string(random()));
		std::filesystem::create_directory(directory);
		try
		{
			auto scene = MakeComponentScene();
			const auto path = directory / "Scene.aster";
			scene.Save(path);
			Check(Scene::Load(path).Serialize() == scene.Serialize(), "file round trip");
			scene.SetName("Replaced file");
			scene.Save(path);
			Check(Scene::Load(path).GetName() == "Replaced file", "saving atomically replaces existing file");
			std::ofstream(path, std::ios::app) << " garbage";
			Rejects([&] { (void)Scene::Load(path); }, "trailing file content rejected");
			std::ofstream(path, std::ios::trunc) << "{ broken";
			Rejects([&] { (void)Scene::Load(path); }, "malformed file rejected");
			Rejects([&] { (void)Scene::Load(directory / "Missing.aster"); }, "missing file rejected");
			Rejects([&] { scene.Save(directory / "Missing" / "Scene.aster"); }, "failed file creation reported");
			std::filesystem::remove_all(directory);
		}
		catch (...)
		{
			std::filesystem::remove_all(directory);
			throw;
		}
	}
} // namespace

void RunSceneTests()
{
	TestEntityLifetime();
	TestHierarchyAndPrefabs();
	TestSerializationAndInput();
	TestFiles();
}
