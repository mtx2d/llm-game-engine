#pragma once

#include <Aster/Input/InputState.h>
#include <Aster/Scene/Scene.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Aster
{
	// Offline uses the same miniaudio graph as Device and permits deterministic PCM tests.
	enum class AudioMode
	{
		Disabled,
		Offline,
		Device
	};

	struct SimulationSettings
	{
		double FixedTimeStep = 1.0 / 60.0;
		std::size_t MaxSubSteps = 8;
		AudioMode Audio = AudioMode::Offline;
		glm::vec3 Gravity{0.0f, -9.81f, 0.0f};
		// Aggregate Lua heap limit, shared by scripts and coroutines; excludes engine assets.
		std::size_t LuaMemoryLimitBytes = 64ULL * 1024ULL * 1024ULL;
	};

	// Owns Lua, Bullet, and miniaudio. Scene must outlive Simulation. All calls are
	// confined to the simulation thread; no window or graphics device is needed.
	// Scripts return a table with optional OnCreate(self, entity),
	// OnUpdate(self, entity, dt), OnDestroy(self, entity), and
	// OnCollision(self, entity, otherEntity, began) functions. Entity values are
	// persistent scene IDs; all operations except exists reject stale IDs.
	// Structural mutations are applied immediately; callbacks iterate snapshots.
	// Entities created inside a callback start scripting on the following step.
	// Errors disable the affected script and are available through GetErrors().
	// Execute() reports errors by throwing std::runtime_error.
	class Simulation
	{
	  public:
		Simulation(Scene& scene, std::filesystem::path assetRoot, SimulationSettings settings = {});
		~Simulation();
		Simulation(const Simulation&) = delete;
		Simulation& operator=(const Simulation&) = delete;

		void Start();
		void Stop();
		bool IsRunning() const;
		void Update(double deltaTime);
		void Step();
		// Window frames may arrive faster than fixed steps. Held values use the
		// latest snapshot; edges, cursor motion, and wheel accumulate until the next
		// step, then appear only once. Lua input queries reflect the last fixed step.
		// Lua: key_down/key_pressed/key_released("Space"), corresponding mouse_
		// queries with "Left"/"Right"/etc.; mouse_position/mouse_delta/mouse_wheel
		// return x,y; input_focused returns a boolean. Invalid names raise Lua errors.
		void SetInput(const InputSnapshot& input);
		void Execute(std::string_view source, std::string_view label = "console");
		const std::vector<std::string>& GetErrors() const;
		const std::vector<std::string>& GetLog() const;
		void ClearErrors();
		void ApplyForce(Entity entity, glm::vec3 force);
		void ApplyImpulse(Entity entity, glm::vec3 impulse);
		void SetVelocity(Entity entity, glm::vec3 velocity);
		glm::vec3 GetVelocity(Entity entity);
		void PlayAudio(Entity entity);
		void StopAudio(Entity entity);
		bool IsAudioPlaying(Entity entity);
		// Stereo interleaved float PCM, only valid with AudioMode::Offline.
		std::vector<float> RenderAudio(std::uint32_t frameCount);

	  private:
		class Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace Aster
