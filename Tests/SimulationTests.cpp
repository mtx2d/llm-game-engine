#include <Aster/Simulation/Simulation.h>
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	using namespace Aster;

	void Check(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error("Simulation: " + message);
		}
	}

	template <typename Function> void Rejects(Function&& function, const std::string& description)
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
		Check(rejected, "did not reject " + description);
	}

	struct TestAssets
	{
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("aster-simulation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

		TestAssets()
		{
			std::filesystem::create_directories(Root / "Scripts");
			std::filesystem::create_directories(Root / "Audio");
			std::filesystem::create_directories(Root / "Prefabs");
			std::filesystem::copy_file(std::filesystem::path(ASTER_SOURCE_DIR) / "Assets/Scripts/FeatureGallery.lua",
									   Root / "Scripts/FeatureGallery.lua");
			Scene prefab("Feature Prefab");
			const auto root = prefab.CreateEntity("Prefab Root");
			const auto child = prefab.CreateEntity("Prefab Child");
			prefab.SetParent(child, root);
			prefab.Save(Root / "Prefabs/Feature.json");
			WriteWave();
		}

		~TestAssets()
		{
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}

		void Script(const std::string& name, const std::string& source) const
		{
			std::ofstream file(Root / "Scripts" / name);
			file << source;
			Check(file.good(), "could not write script fixture");
		}

		void WriteWave() const
		{
			std::ofstream file(Root / "Audio/Feature.wav", std::ios::binary);
			auto write = [&](std::uint32_t value, int bytes)
			{
				for (int index = 0; index < bytes; ++index)
				{
					file.put(static_cast<char>((value >> (index * 8)) & 255));
				}
			};
			constexpr std::uint32_t frames = 4800;
			file.write("RIFF", 4);
			write(36 + frames * 2, 4);
			file.write("WAVEfmt ", 8);
			write(16, 4);
			write(1, 2);
			write(1, 2);
			write(48000, 4);
			write(96000, 4);
			write(2, 2);
			write(16, 2);
			file.write("data", 4);
			write(frames * 2, 4);
			for (std::uint32_t frame = 0; frame < frames; ++frame)
			{
				const auto sample =
					static_cast<std::int16_t>(12000.0 * std::sin(static_cast<double>(frame) * 0.0575958653));
				write(static_cast<std::uint16_t>(sample), 2);
			}
			Check(file.good(), "could not write audio fixture");
		}
	};

	void AssertNoScriptErrors(const Simulation& simulation)
	{
		if (!simulation.GetErrors().empty())
		{
			throw std::runtime_error("Unexpected Lua error: " + simulation.GetErrors().front());
		}
	}

	Entity AddBody(Scene& scene, const std::string& name, glm::vec3 position, BodyType type)
	{
		const auto entity = scene.CreateEntity(name);
		auto& data = scene.Get(entity);
		data.Transform.Translation = position;
		data.RigidBody = RigidBodyComponent{};
		data.RigidBody->Type = type;
		return entity;
	}

	void TestPhysics(const TestAssets& assets)
	{
		Scene scene;
		const auto floor = AddBody(scene, "Floor", {0, -0.5f, 0}, BodyType::Static);
		scene.Get(floor).RigidBody->HalfExtents = {20, 0.5f, 20};
		const auto cube = AddBody(scene, "Cube", {0, 4, 0}, BodyType::Dynamic);
		scene.Get(cube).RigidBody->Mass = 2;
		Simulation simulation(scene, assets.Root, {.Audio = AudioMode::Disabled});
		Rejects([&] { simulation.Step(); }, "stepping a stopped simulation");
		simulation.Start();
		Rejects([&] { simulation.Start(); }, "double start");
		Rejects([&] { simulation.Update(-1); }, "negative dt");
		Rejects([&] { simulation.Update(std::numeric_limits<double>::infinity()); }, "infinite dt");
		simulation.Update(1.0 / 120.0);
		Check(scene.Get(cube).Transform.Translation.y == 4.0f, "half step advanced physics");
		simulation.Update(1.0 / 120.0);
		Check(scene.Get(cube).Transform.Translation.y < 4.0f, "gravity did not advance after accumulated step");
		for (int frame = 0; frame < 240; ++frame)
		{
			simulation.Step();
		}
		Check(std::abs(scene.Get(cube).Transform.Translation.y - 0.5f) < 0.02f,
			  "cube did not settle on actual Bullet collider");
		simulation.ApplyImpulse(cube, {4, 0, 0});
		Check(std::abs(simulation.GetVelocity(cube).x - 2.0f) < 0.001f, "impulse ignored mass");
		simulation.SetVelocity(cube, {0, 2, 0});
		const float previous = scene.Get(cube).Transform.Translation.y;
		simulation.Step();
		Check(scene.Get(cube).Transform.Translation.y > previous, "velocity did not move rigidbody");
		Rejects([&] { simulation.ApplyForce(floor, {1, 0, 0}); }, "force on a static body");
		Rejects([&] { simulation.SetVelocity(cube, {std::numeric_limits<float>::quiet_NaN(), 0, 0}); }, "NaN velocity");
		scene.DestroyEntity(cube);
		simulation.Step();
		Rejects([&] { (void)simulation.GetVelocity(cube); }, "stale body");
		simulation.Stop();
		Check(!simulation.IsRunning(), "Stop did not change state");
		simulation.Start();
		simulation.Step();
		simulation.Stop();
	}

	void TestDeterminismAndHierarchy(const TestAssets& assets)
	{
		Scene first;
		Scene second;
		const auto firstBody = AddBody(first, "Ball", {0, 4, 0}, BodyType::Dynamic);
		const auto secondBody = AddBody(second, "Ball", {0, 4, 0}, BodyType::Dynamic);
		first.Get(firstBody).RigidBody->Shape = CollisionShape::Sphere;
		second.Get(secondBody).RigidBody->Shape = CollisionShape::Sphere;
		Simulation firstSimulation(first, assets.Root, {.Audio = AudioMode::Disabled});
		Simulation secondSimulation(second, assets.Root, {.Audio = AudioMode::Disabled});
		firstSimulation.Start();
		secondSimulation.Start();
		for (int frame = 0; frame < 60; ++frame)
		{
			firstSimulation.Update(1.0 / 60.0);
			secondSimulation.Update(1.0 / 120.0);
			secondSimulation.Update(1.0 / 120.0);
		}
		Check(glm::length(first.Get(firstBody).Transform.Translation - second.Get(secondBody).Transform.Translation) <
				  0.000001f,
			  "fixed-step integration depends on frame partition");
		const auto parent = first.CreateEntity("Parent");
		first.Get(parent).Transform.Translation = {0, 10, 0};
		first.SetParent(firstBody, parent);
		first.Get(firstBody).Transform.Translation = {0, 2, 0};
		firstSimulation.SetVelocity(firstBody, {0, 0, 0});
		firstSimulation.Step();
		Check(first.Get(firstBody).Transform.Translation.y < 2.0f &&
				  first.Get(firstBody).Transform.Translation.y > 1.9f,
			  "physics failed to derive local transform from parent");
		first.Get(parent).RigidBody = RigidBodyComponent{};
		Rejects([&] { firstSimulation.Step(); }, "body hierarchy without constraints");
		first.Get(parent).RigidBody.reset();
		first.Get(firstBody).Transform.Scale = {1, 2, 1};
		Rejects([&] { firstSimulation.Step(); }, "nonuniform sphere collider");
	}

	void TestLuaBoundaries(const TestAssets& assets)
	{
		Scene scene;
		Simulation simulation(scene, assets.Root, {.Audio = AudioMode::Disabled});
		simulation.Execute(R"(
			local entity = engine.create("Boundary")
			engine.set_position(entity, 1, 2, 3)
			assert(not pcall(engine.set_position, entity, 0/0, 0, 0))
			local x, y, z = engine.get_position(entity)
			assert(x == 1 and y == 2 and z == 3)
			assert(not pcall(engine.set_component, entity, "Transform", {Scale = {0, 1, 1}}))
			assert(not pcall(engine.set_component, entity, "Camera", {NearClip = 100, FarClip = 2}))
			assert(not pcall(engine.set_component, entity, "RigidBody", {Mass = -1}))
			assert(not pcall(engine.set_component, entity, "Light", {Type = "Invalid"}))
			assert(not pcall(engine.set_component, entity, "Camera", {Typo = true}))
			assert(not pcall(engine.set_component, entity, "Script", {Path = "../secret.lua"}))
			assert(not pcall(engine.set_component, entity, "AudioSource", {Path = "/outside.wav"}))
			assert(not pcall(engine.set_component, entity, "MeshRenderer", {Mesh = "../escape.gltf"}))
			assert(not pcall(engine.set_component, entity, "Light", {Intensity = math.huge}))
			assert(not pcall(engine.set_component, entity, "Transform", {Translation = {1, 2}}))
			assert(not pcall(engine.set_component, entity, "Transform", {Translation = {1, 2, 3, typo = true}}))
			assert(not pcall(engine.get_component, entity, "Typo"))
			assert(not pcall(engine.remove_component, entity, "Transform"))
			assert(not pcall(engine.set_parent, entity, entity))
			assert(not pcall(engine.exists, 1.5))
			assert(not pcall(engine.exists, "1"))
			assert(not pcall(engine.spawn_prefab, "../escape.json"))
			assert(not pcall(engine.spawn_prefab, "missing.json"))
			engine.destroy(entity)
			assert(not engine.exists(entity))
			assert(not pcall(engine.destroy, entity))
			assert(not pcall(engine.get_position, entity))
			assert(io == nil and os == nil and package == nil and debug == nil)
			coroutine.wrap(function()
				local child = engine.create("Coroutine entity")
				assert(engine.get_name(child) == "Coroutine entity")
				assert(not pcall(engine.set_position, child, "bad", 0, 0))
				engine.destroy(child)
			end)()
			assert(engine.find("Coroutine entity") == nil)
		)");
		Rejects([&] { simulation.Execute("error('expected failure')"); }, "Lua exception");
		Rejects([&] { simulation.Execute("while true do end"); }, "instruction budget");
		Rejects([&] { simulation.Execute("while true do pcall(function() while true do end end) end"); },
				"instruction budget caught in pcall loop");
		Rejects(
			[&] {
				simulation.Execute(
					"while true do xpcall(function() while true do end end, function(e) return e end) end");
			},
			"instruction budget caught in xpcall loop");
		Rejects(
			[&] {
				simulation.Execute(
					"while true do coroutine.resume(coroutine.create(function() while true do end end)) end");
			},
			"instruction budget caught in coroutine loop");
		Rejects(
			[&] {
				simulation.Execute(
					"local item <close> = setmetatable({}, {__close = function() while true do end end})");
			},
			"instruction budget in __close");
		Rejects(
			[&]
			{
				simulation.Execute("local item <close> = setmetatable({}, {__close = function() while true do "
								   "pcall(function() while true do end end) end end}); error('close while unwinding')");
			},
			"instruction budget in __close while unwinding");
		Rejects([&] { simulation.Execute("this is not Lua"); }, "syntax error");
		simulation.Execute("assert(1 + 1 == 2)");
		Check(scene.Size() == 0, "Lua destruction left a live entity");
	}

	void TestForceAndKinematicBody(const TestAssets& assets)
	{
		Scene forceScene;
		const auto body = AddBody(forceScene, "Forced", {0, 0, 0}, BodyType::Dynamic);
		forceScene.Get(body).RigidBody->Mass = 2;
		Simulation forceSimulation(forceScene, assets.Root, {.Audio = AudioMode::Disabled, .Gravity = {0, 0, 0}});
		forceSimulation.Start();
		forceSimulation.ApplyForce(body, {12, 0, 0});
		forceSimulation.Step();
		Check(std::abs(forceSimulation.GetVelocity(body).x - 0.1f) < 0.00001f, "force integration ignored mass or dt");
		forceSimulation.Step();
		Check(std::abs(forceSimulation.GetVelocity(body).x - 0.1f) < 0.00001f, "force was not cleared after a step");

		Scene kinematicScene;
		const auto platform = AddBody(kinematicScene, "Platform", {0, -0.5f, 0}, BodyType::Kinematic);
		kinematicScene.Get(platform).RigidBody->HalfExtents = {5, 0.5f, 5};
		const auto riding = AddBody(kinematicScene, "Riding", {0, 1, 0}, BodyType::Dynamic);
		Simulation kinematicSimulation(kinematicScene, assets.Root, {.Audio = AudioMode::Disabled});
		kinematicSimulation.Start();
		for (int frame = 0; frame < 90; ++frame)
		{
			kinematicSimulation.Step();
		}
		for (int frame = 0; frame < 120; ++frame)
		{
			kinematicScene.Get(platform).Transform.Translation.y += 0.01f;
			kinematicSimulation.Step();
		}
		Check(kinematicScene.Get(riding).Transform.Translation.y > 1.5f,
			  "moving kinematic collider did not lift dynamic body");
		Check(std::abs(kinematicScene.Get(platform).Transform.Translation.y - 0.7f) < 0.001f,
			  "physics overwrote authored kinematic transform");
	}

	void TestMutationAndReload(const TestAssets& assets)
	{
		assets.Script("Mutation.lua", R"(
			return {
				OnCreate = function(self, entity) engine.log("create") end,
				OnUpdate = function(self, entity, dt)
					assert(dt > 0)
					local child = engine.create("Spawned")
					engine.set_component(child, "Script", {Path = "Scripts/Child.lua"})
					engine.destroy(entity)
				end,
				OnDestroy = function(self, entity) assert(not engine.exists(entity)); engine.log("destroy") end
			}
		)");
		assets.Script("Child.lua", R"(
			return {
				OnCreate = function(self, entity) engine.log("child create") end,
				OnUpdate = function(self, entity, dt) engine.log("child update") end,
				OnDestroy = function(self, entity) engine.log("child destroy") end
			}
		)");
		assets.Script("Broken.lua", "return {OnUpdate = function() error('intentional') end}");
		assets.Script("Reloaded.lua", "return {OnCreate = function() engine.log('reloaded') end}");
		Scene scene;
		const auto entity = scene.CreateEntity("Mutation");
		scene.Get(entity).Script = {"Scripts/Mutation.lua", true};
		Simulation simulation(scene, assets.Root, {.Audio = AudioMode::Disabled});
		simulation.Start();
		Check(simulation.GetLog() == std::vector<std::string>{"create"}, "OnCreate not called exactly once");
		simulation.Step();
		Check(!scene.IsAlive(entity) && scene.Size() == 1, "callback mutation failed");
		Check(simulation.GetLog().size() == 1, "new script updated during creation frame");
		simulation.Step();
		Check(simulation.GetLog() == std::vector<std::string>{"create", "destroy", "child create", "child update"},
			  "lifecycle order is incorrect");
		const auto child = scene.Entities().front();
		scene.Get(child).Script->Enabled = false;
		simulation.Step();
		Check(simulation.GetLog().back() == "child destroy", "disabling script did not release lifecycle");
		scene.Get(child).Script = {"Scripts/Broken.lua", true};
		simulation.Step();
		Check(simulation.GetErrors().size() == 1 && simulation.GetErrors()[0].find("intentional") != std::string::npos,
			  "script error lacks context");
		simulation.Step();
		Check(simulation.GetErrors().size() == 1, "failed script continued executing");
		scene.Get(child).Script->Path = "Scripts/Reloaded.lua";
		simulation.Step();
		Check(simulation.GetLog().back() == "reloaded", "changed script did not reload");
		simulation.ClearErrors();
		AssertNoScriptErrors(simulation);
		simulation.Stop();
	}

	void TestCollisionCallbacks(const TestAssets& assets)
	{
		assets.Script("Collision.lua", R"(
			return {
				OnCollision = function(self, entity, other, began)
					if began then
						assert(engine.exists(other))
						engine.log("begin")
						engine.set_position(entity, 0, 20, 0)
						engine.set_velocity(entity, 0, 0, 0)
					else
						engine.log("end")
						engine.destroy(entity)
					end
				end
			}
		)");
		Scene scene;
		const auto floor = AddBody(scene, "Floor", {0, -0.5f, 0}, BodyType::Static);
		scene.Get(floor).RigidBody->HalfExtents = {5, 0.5f, 5};
		const auto body = AddBody(scene, "Trigger", {0, 0.6f, 0}, BodyType::Dynamic);
		scene.Get(body).RigidBody->Shape = CollisionShape::Capsule;
		scene.Get(body).RigidBody->IsTrigger = true;
		scene.Get(body).Script = {"Scripts/Collision.lua", true};
		Simulation simulation(scene, assets.Root, {.Audio = AudioMode::Disabled});
		simulation.Start();
		for (int frame = 0; frame < 30 && scene.IsAlive(body); ++frame)
		{
			simulation.Step();
		}
		AssertNoScriptErrors(simulation);
		Check(!scene.IsAlive(body), "collision callback did not safely destroy body");
		Check(simulation.GetLog() == std::vector<std::string>{"begin", "end"}, "contact transitions are incorrect");
		simulation.Step();
	}

	void TestSelfRemovalAndDestroyMutation(const TestAssets& assets)
	{
		assets.Script("RemoveSelf.lua", R"(
			return {
				OnCreate = function(self, entity)
					engine.set_component(entity, "RigidBody", {Type = "Dynamic"})
					engine.set_component(entity, "AudioSource", {Path = "Audio/Feature.wav", Loop = true})
					engine.play_audio(entity)
				end,
				OnUpdate = function(self, entity)
					engine.remove_component(entity, "RigidBody")
					engine.remove_component(entity, "AudioSource")
					engine.remove_component(entity, "Script")
					engine.log("removed self")
				end,
				OnDestroy = function(self, entity)
					assert(engine.exists(entity))
					engine.destroy(entity)
					local spawned = engine.create("OnDestroy child")
					engine.set_component(spawned, "Script", {Path = "Scripts/DestroyChild.lua"})
					engine.log("destroyed self")
				end
			}
		)");
		assets.Script("DestroyChild.lua", R"(
			return {
				OnCreate = function(self, entity) engine.log("destroy child created") end,
				OnDestroy = function(self, entity)
					engine.destroy(entity)
					engine.log("stop destroyed child")
				end
			}
		)");
		Scene scene;
		const auto entity = scene.CreateEntity("Self removal");
		scene.Get(entity).Script = {"Scripts/RemoveSelf.lua", true};
		Simulation simulation(scene, assets.Root);
		simulation.Start();
		simulation.Step();
		Check(!scene.Get(entity).RigidBody && !scene.Get(entity).AudioSource && !scene.Get(entity).Script,
			  "callback failed to remove its components");
		simulation.Step();
		AssertNoScriptErrors(simulation);
		Check(!scene.IsAlive(entity) && scene.Size() == 1, "OnDestroy could not replace its own entity");
		Check(simulation.GetLog() == std::vector<std::string>{"removed self", "destroyed self"},
			  "OnDestroy-created script started in the same step");
		simulation.Step();
		Check(simulation.GetLog() ==
				  std::vector<std::string>{"removed self", "destroyed self", "destroy child created"},
			  "OnDestroy mutation lifecycle mismatch");
		simulation.Stop();
		AssertNoScriptErrors(simulation);
		Check(scene.Size() == 0 && simulation.GetLog().back() == "stop destroyed child", "stop callback leaked entity");
	}

	void TestAudio(const TestAssets& assets)
	{
		Scene scene;
		const auto listener = scene.CreateEntity("Listener");
		scene.Get(listener).Camera = CameraComponent{};
		const auto source = scene.CreateEntity("Source");
		scene.Get(source).AudioSource = {"Audio/Feature.wav", 0.8f, 1.0f, false, false, false};
		Simulation simulation(scene, assets.Root);
		simulation.Start();
		Check(!simulation.IsAudioPlaying(source), "audio played without PlayOnStart");
		simulation.PlayAudio(source);
		Check(simulation.IsAudioPlaying(source), "audio start failed");
		const auto samples = simulation.RenderAudio(2048);
		Check(std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 0.01f; }),
			  "real miniaudio PCM output is silent");
		Check(std::all_of(samples.begin(), samples.end(), [](float sample) { return std::isfinite(sample); }),
			  "nonfinite PCM");
		simulation.StopAudio(source);
		Check(!simulation.IsAudioPlaying(source), "audio stop failed");
		(void)simulation.RenderAudio(512); // allow resampler/smoothing tail to drain
		const auto silent = simulation.RenderAudio(512);
		Check(std::all_of(silent.begin(), silent.end(), [](float sample) { return std::abs(sample) < 0.00001f; }),
			  "stopped audio still produces PCM");
		scene.Get(source).AudioSource->Loop = true;
		simulation.PlayAudio(source);
		(void)simulation.RenderAudio(9600);
		Check(simulation.IsAudioPlaying(source), "looping sound stopped at EOF");
		scene.Get(source).AudioSource->Volume = 0;
		(void)simulation.RenderAudio(1024);
		const auto muted = simulation.RenderAudio(1024);
		Check(std::all_of(muted.begin(), muted.end(), [](float sample) { return std::abs(sample) < 0.00001f; }),
			  "volume not applied");
		scene.DestroyEntity(source);
		simulation.Step();
		Rejects([&] { simulation.PlayAudio(source); }, "stale audio source");
		Rejects([&] { (void)simulation.RenderAudio(0); }, "zero frame audio block");
	}

	void TestAudioValidationScene()
	{
		const auto assetRoot = std::filesystem::path(ASTER_SOURCE_DIR) / "Assets";
		auto scene = Scene::Load(assetRoot / "Scenes/AudioValidation.aster");
		const auto listener = scene.FindByID(1);
		const auto source = scene.FindByID(2);
		Check(scene.Get(listener).Camera && scene.Get(listener).Camera->Primary &&
				  scene.Get(listener).Transform.Translation == glm::vec3(0) &&
				  scene.Get(listener).Transform.Rotation == glm::vec3(0),
			  "audio fixture listener must stay at the origin facing -Z");
		Check(scene.Get(source).AudioSource && scene.Get(source).AudioSource->Spatial &&
				  scene.Get(source).AudioSource->Loop && scene.Get(source).AudioSource->PlayOnStart,
			  "audio fixture must start a looping spatial source");
		Simulation simulation(scene, assetRoot, {.FixedTimeStep = 1.0 / 60.0, .Audio = AudioMode::Offline});
		simulation.Start();
		AssertNoScriptErrors(simulation);
		Check(simulation.IsAudioPlaying(source), "audio fixture PlayOnStart did not start playback");

		struct ChannelLevels
		{
			double Left = 0;
			double Right = 0;
			double Peak = 0;
		};
		std::array<ChannelLevels, 6> levels{};
		const std::array<std::string, 6> stages = {"LEFT", "CENTER", "RIGHT", "FAR", "SILENT", "LEFT"};
		for (std::size_t stage = 0; stage < stages.size(); ++stage)
		{
			auto& level = levels[stage];
			std::size_t measuredFrames = 0;
			for (std::size_t step = 0; step < 120; ++step)
			{
				Check(scene.Get(source).Name == "AudioValidation: " + stages[stage],
					  "audio fixture changed stage before or after its two-second boundary");
				// 800 stereo frames at 48 kHz exactly match one fixed simulation step.
				const auto samples = simulation.RenderAudio(800);
				Check(samples.size() == 1600, "audio fixture PCM block has an unexpected channel count");
				for (std::size_t frame = 0; frame < 800; ++frame)
				{
					const double left = samples[frame * 2];
					const double right = samples[frame * 2 + 1];
					Check(std::isfinite(left) && std::isfinite(right), "audio fixture produced nonfinite PCM");
					// Exclude the short gain/pan smoothing tail after each stage transition.
					if (step >= 4)
					{
						level.Left += left * left;
						level.Right += right * right;
						level.Peak = std::max({level.Peak, std::abs(left), std::abs(right)});
						++measuredFrames;
					}
				}
				simulation.Step();
			}
			level.Left = std::sqrt(level.Left / static_cast<double>(measuredFrames));
			level.Right = std::sqrt(level.Right / static_cast<double>(measuredFrames));
			AssertNoScriptErrors(simulation);
			Check(simulation.IsAudioPlaying(source), "audio fixture loop stopped during a stage");
		}
		const auto& left = levels[0];
		const auto& center = levels[1];
		const auto& right = levels[2];
		const auto& far = levels[3];
		const auto& silent = levels[4];
		Check(center.Left > 0.005 && center.Right > 0.005 && center.Peak < 0.25,
			  "audio fixture center should be audible at a moderate level");
		Check(left.Left > left.Right * 1.5 && right.Right > right.Left * 1.5,
			  "spatial audio fixture does not favor the expected left/right channels");
		Check(std::abs(center.Left - center.Right) < center.Left * 0.02,
			  "centered audio fixture has unbalanced stereo channels");
		Check(std::abs(left.Left - right.Right) < left.Left * 0.03 &&
				  std::abs(left.Right - right.Left) < right.Left * 0.03,
			  "mirrored audio positions did not produce mirrored channel levels");
		Check(far.Left > center.Left * 0.02 && far.Left < center.Left * 0.25 && far.Right > center.Right * 0.02 &&
				  far.Right < center.Right * 0.25,
			  "distant audio fixture must be quieter, with nonzero attenuated output");
		Check(silent.Peak < 0.000001, "audio fixture silent stage still produces PCM");
		Check(std::abs(levels[5].Left - left.Left) < left.Left * 0.03 &&
				  std::abs(levels[5].Right - left.Right) < left.Right * 0.03,
			  "audio fixture did not restore spatial playback after its silent stage");
		Check(scene.Get(source).Name == "AudioValidation: CENTER" && simulation.GetLog().size() == 7,
			  "audio fixture did not repeat the complete five-stage sequence");
		simulation.Stop();
		simulation.Start();
		AssertNoScriptErrors(simulation);
		Check(scene.Get(source).Name == "AudioValidation: LEFT" && simulation.IsAudioPlaying(source),
			  "restarting the audio fixture did not reset its stage and playback");
	}

	void TestFeatureGallery(const TestAssets& assets)
	{
		Scene scene;
		const auto gallery = scene.CreateEntity("Feature Gallery");
		scene.Get(gallery).Script = {"Scripts/FeatureGallery.lua", true};
		Simulation simulation(scene, assets.Root);
		simulation.Start();
		AssertNoScriptErrors(simulation);
		Check(scene.Size() == 1, "feature API probe leaked entities/prefab children");
		Check(simulation.GetLog().size() == 1 && simulation.GetLog()[0].find("all 32") != std::string::npos,
			  "feature API coverage did not finish");
		simulation.Step();
		AssertNoScriptErrors(simulation);
		Check(scene.Get(gallery).Transform.Rotation.y > 0, "feature update did not animate transform");
		simulation.Stop();
		Check(simulation.GetLog().back() == "FeatureGallery: destroyed", "feature OnDestroy missing");
	}

	void TestInputEvents(const TestAssets& assets)
	{
		InputState input;
		input.BeginFrame();
		input.KeyEvent("Space", true);
		input.KeyEvent("Space", false);
		input.KeyEvent("W", true);
		input.MouseButtonEvent(0, true);
		input.MouseButtonEvent(0, false);
		input.MouseButtonEvent(1, true);
		input.CursorEvent(100, 200);
		input.CursorEvent(105, 197);
		input.ScrollEvent(1, 2);
		const auto first = input.GetSnapshot();
		InputState::Validate(first);
		Check(first.KeysPressed.contains("Space") && first.KeysReleased.contains("Space") &&
				  !first.KeysDown.contains("Space"),
			  "quick key tap lost an edge");
		Check(first.KeysDown.contains("W") && first.MousePressed[0] && first.MouseReleased[0] && !first.MouseDown[0],
			  "held key or quick mouse tap incorrect");
		Check(first.MouseDelta == glm::vec2(5, -3) && first.MousePosition == glm::vec2(105, 197),
			  "mouse event accumulation incorrect");
		input.BeginFrame();
		input.KeyEvent("W", true);
		Check(input.GetSnapshot().KeysPressed.empty() && input.GetSnapshot().KeysDown.contains("W"),
			  "key repeat created another press");
		input.FocusEvent(false);
		InputState::Validate(input.GetSnapshot());
		Check(input.GetSnapshot().KeysReleased.contains("W") && input.GetSnapshot().KeysDown.empty() &&
				  input.GetSnapshot().MouseReleased[1] && !input.GetSnapshot().Focused,
			  "focus loss did not release held input");
		input.KeyEvent("A", true);
		Check(input.GetSnapshot().KeysDown.empty(), "unfocused key press was accepted");
		input.FocusEvent(true);
		input.CursorEvent(900, 700);
		Check(input.GetSnapshot().MouseDelta == glm::vec2(0), "focus regain caused a cursor jump");
		Rejects([&] { input.KeyEvent("NotAKey", true); }, "unknown key name");
		Rejects([&] { input.MouseButtonEvent(8, true); }, "invalid mouse button");
		Rejects([&] { input.CursorEvent(std::numeric_limits<double>::infinity(), 0); }, "nonfinite cursor event");
		Rejects([&] { input.ScrollEvent(0, std::numeric_limits<double>::quiet_NaN()); }, "nonfinite wheel event");
		Rejects([&] { (void)InputState::MouseButtonIndex("Unknown"); }, "unknown mouse button name");

		assets.Script("InputProbe.lua", R"(
			return {
				OnCreate = function(self) self.steps = 0 end,
				OnUpdate = function(self)
					self.steps = self.steps + 1
					if self.steps == 1 then
						assert(engine.key_pressed("Space") and engine.key_released("Space") and not engine.key_down("Space"))
						assert(engine.key_pressed("W") and engine.key_down("W"))
						assert(engine.mouse_pressed("Left") and engine.mouse_released("Left") and not engine.mouse_down("Left"))
						assert(engine.mouse_down("Right") and engine.input_focused())
						local x, y = engine.mouse_position(); assert(x == 108 and y == 199)
						local dx, dy = engine.mouse_delta(); assert(dx == 8 and dy == -1)
						local wx, wy = engine.mouse_wheel(); assert(wx == 3 and wy == 1)
						engine.log("input edges")
					elseif self.steps == 2 then
						assert(engine.key_down("W") and not engine.key_pressed("W"))
						assert(not engine.key_pressed("Space") and not engine.key_released("Space"))
						assert(engine.mouse_down("Right") and not engine.mouse_pressed("Right"))
						assert(not engine.mouse_pressed("Left") and not engine.mouse_released("Left"))
						local dx, dy = engine.mouse_delta(); assert(dx == 0 and dy == 0)
						local wx, wy = engine.mouse_wheel(); assert(wx == 0 and wy == 0)
						engine.log("input held")
					elseif self.steps == 3 then
						assert(not engine.input_focused() and not engine.key_down("W") and engine.key_released("W"))
						assert(not engine.mouse_down("Right") and engine.mouse_released("Right"))
						engine.log("input focus released")
					end
				end
			}
		)");
		Scene scene;
		const auto entity = scene.CreateEntity("Input probe");
		scene.Get(entity).Script = {"Scripts/InputProbe.lua", true};
		Simulation simulation(scene, assets.Root, {.Audio = AudioMode::Disabled});
		simulation.Start();
		simulation.SetInput(first);
		simulation.Update(1.0 / 240.0);
		Check(simulation.GetLog().empty(), "input was consumed before fixed timestep");
		InputSnapshot next;
		next.KeysDown.insert("W");
		next.MouseDown[1] = true;
		next.MousePosition = {108, 199};
		next.MouseDelta = {3, 2};
		next.Wheel = {2, -1};
		simulation.SetInput(next);
		simulation.Update(7.0 / 240.0); // Two fixed steps; edges appear in the first only.
		AssertNoScriptErrors(simulation);
		Check(simulation.GetLog() == std::vector<std::string>{"input edges", "input held"},
			  "fixed-step input delivery incorrect");
		InputSnapshot unfocused;
		unfocused.Focused = false;
		simulation.SetInput(unfocused); // Held-state transitions also synthesize release edges.
		simulation.Step();
		AssertNoScriptErrors(simulation);
		Check(simulation.GetLog().back() == "input focus released", "simulation did not release input on focus loss");
		InputSnapshot invalid;
		invalid.KeysPressed.insert("W");
		Rejects([&] { simulation.SetInput(invalid); }, "contradictory input snapshot");
		invalid = {};
		invalid.MousePosition.x = std::numeric_limits<float>::quiet_NaN();
		Rejects([&] { simulation.SetInput(invalid); }, "NaN snapshot coordinates");
		simulation.Execute(R"(
			assert(not pcall(engine.key_down, "w"))
			assert(not pcall(engine.key_pressed, "Unknown"))
			assert(not pcall(engine.mouse_down, "Unknown"))
			assert(not pcall(engine.mouse_released, 0))
		)");
	}
	void TestLuaMemoryLimits(const TestAssets& assets)
	{
		Scene scene;
		const auto probe = scene.CreateEntity("Memory probe");
		scene.Get(probe).MeshRenderer.emplace();
		scene.Get(probe).MeshRenderer->Mesh = "Models/Triangle.gltf";
		SimulationSettings settings;
		settings.Audio = AudioMode::Disabled;
		settings.LuaMemoryLimitBytes = 1024 * 1024;
		Simulation simulation(scene, assets.Root, settings);
		Rejects([&] { simulation.Execute("return string.rep('x', 8 * 1024 * 1024)"); },
				"Lua memory limit on one C-library allocation");
		simulation.Execute("assert(engine.find('Memory probe') ~= nil); engine.log('after allocation failure')");
		Check(simulation.GetLog().back() == "after allocation failure",
			  "console did not recover after allocation failure");
		// Exhaust memory while preparing engine return tables on a coroutine stack.
		// Its failure must restore the host state and unwind C++ binding locals.
		simulation.Execute(R"(
			local thread = coroutine.create(function()
				local retained = {}
				local probe = engine.find("Memory probe")
				for i = 1, 100000 do retained[i] = engine.get_component(probe, "MeshRenderer") end
			end)
			local success, failure = coroutine.resume(thread)
			assert(not success and string.find(failure, "memory"))
			thread = nil
			collectgarbage("collect")
			assert(engine.get_name(engine.find("Memory probe")) == "Memory probe")
			engine.log("coroutine memory recovered")
		)");
		Check(simulation.GetLog().back() == "coroutine memory recovered", "coroutine OOM corrupted the host Lua state");
		assets.Script("MemoryFailure.lua",
					  "return { OnUpdate = function() local unused = string.rep('x', 8 * 1024 * 1024) end }");
		assets.Script("MemorySurvivor.lua", R"(
			return {
				OnCreate = function(self) self.count = 0; engine.log("survivor created") end,
				OnUpdate = function(self) self.count = self.count + 1; engine.log("survivor " .. self.count) end
			}
		)");
		const auto failed = scene.CreateEntity("Fails memory");
		scene.Get(failed).Script = {"Scripts/MemoryFailure.lua", true};
		const auto survivor = scene.CreateEntity("Survives memory");
		scene.Get(survivor).Script = {"Scripts/MemorySurvivor.lua", true};
		simulation.Start();
		simulation.Step();
		simulation.Step();
		Check(simulation.GetErrors().size() == 1 && simulation.GetErrors()[0].find("memory") != std::string::npos,
			  "memory exhaustion did not disable only the failing script with a descriptive error");
		Check(std::count(simulation.GetLog().begin(), simulation.GetLog().end(), "survivor created") == 1 &&
				  simulation.GetLog().back() == "survivor 2",
			  "memory error restarted or stopped an independent script");
		simulation.Stop();
		simulation.Execute(R"(
			assert(not pcall(setmetatable, {}, { __gc = function() while true do end end }))
			assert(not pcall(setmetatable, {}))
			local ordinary = setmetatable({}, { __index = { Value = 42 } })
			assert(ordinary.Value == 42)
			assert(setmetatable(ordinary, nil) == ordinary)
		)");
		settings.LuaMemoryLimitBytes = 1024;
		Rejects([&] { Simulation invalid(scene, assets.Root, settings); }, "unusable Lua memory budget");
	}

} // namespace

void RunSimulationTests()
{
	// Exercise the implementation linked into Aster: fallback to a silent device
	// must be unavailable even when explicitly requested. Offline mixing is tested below.
	ma_context context{};
	const ma_backend silentBackend = ma_backend_null;
	const auto result = ma_context_init(&silentBackend, 1, nullptr, &context);
	if (result == MA_SUCCESS)
	{
		ma_context_uninit(&context);
	}
	Check(result == MA_NO_BACKEND, "device audio accepted a silent fallback backend");
	const TestAssets assets;
	TestPhysics(assets);
	TestDeterminismAndHierarchy(assets);
	TestLuaBoundaries(assets);
	TestForceAndKinematicBody(assets);
	TestMutationAndReload(assets);
	TestCollisionCallbacks(assets);
	TestSelfRemovalAndDestroyMutation(assets);
	TestAudio(assets);
	TestAudioValidationScene();
	TestFeatureGallery(assets);
	TestInputEvents(assets);
	TestLuaMemoryLimits(assets);
}
