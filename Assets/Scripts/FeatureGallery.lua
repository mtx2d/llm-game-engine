-- Executed by the automated feature scene. Component tables use the public
-- C++ field names; vectors are arrays and enum values are case-sensitive.
return {
    OnCreate = function(self, entity)
        assert(engine.exists(entity))
        local probe = engine.create("API Probe")
        assert(engine.find("API Probe") == probe)
        engine.set_name(probe, "Renamed Probe")
        assert(engine.get_name(probe) == "Renamed Probe")
        engine.set_parent(probe, entity)
        assert(engine.get_parent(probe) == entity)
        engine.set_parent(probe, nil)
        assert(engine.get_parent(probe) == nil)

        engine.set_component(probe, "Transform", {
            Translation = {2, 3, 4}, Rotation = {0.1, 0.2, 0.3}, Scale = {1, 1, 1}
        })
        assert(engine.get_component(probe, "Transform").Scale[1] == 1)
        engine.set_position(probe, 1, 2, 3)
        local x, y, z = engine.get_position(probe)
        assert(x == 1 and y == 2 and z == 3)

        engine.set_component(probe, "Camera", {
            VerticalFov = 65, NearClip = 0.2, FarClip = 250, Primary = false
        })
        assert(engine.get_component(probe, "Camera").FarClip == 250)
        engine.set_component(probe, "MeshRenderer", {
            Mesh = "Models/Cube.gltf", BaseColor = {0.8, 0.3, 0.2, 1},
            Metallic = 0.7, Roughness = 0.25, Visible = true
        })
        assert(engine.get_component(probe, "MeshRenderer").Roughness == 0.25)
        engine.set_component(probe, "Light", {
            Type = "Spot", Color = {1, 0.8, 0.6}, Intensity = 3,
            Range = 8, InnerCone = 15, OuterCone = 25, CastShadows = true
        })
        assert(engine.get_component(probe, "Light").Type == "Spot")
        engine.set_component(probe, "RigidBody", {
            Type = "Dynamic", Shape = "Box", HalfExtents = {0.5, 0.5, 0.5},
            LinearVelocity = {0, 0, 0}, Radius = 0.5, Height = 1,
            Mass = 2, Friction = 0.6, Restitution = 0.1, IsTrigger = false
        })
        assert(engine.get_component(probe, "RigidBody").Mass == 2)
        engine.set_velocity(probe, 1, 0, 0)
        engine.apply_impulse(probe, 2, 0, 0)
        local vx = engine.get_velocity(probe)
        assert(math.abs(vx - 2) < 0.001)
        engine.apply_force(probe, 0, 1, 0)

        engine.set_component(probe, "Script", {Path = "Scripts/FeatureGallery.lua", Enabled = false})
        assert(not engine.get_component(probe, "Script").Enabled)
        engine.set_component(probe, "AudioSource", {
            Path = "Audio/Feature.wav", Volume = 0.25, Pitch = 1.5,
            Loop = true, PlayOnStart = false, Spatial = false
        })
        assert(engine.get_component(probe, "AudioSource").Pitch == 1.5)
        engine.play_audio(probe)
        assert(engine.is_audio_playing(probe))
        engine.stop_audio(probe)
        assert(not engine.is_audio_playing(probe))

        local prefab = engine.spawn_prefab("Prefabs/Feature.json", entity)
        assert(engine.get_parent(prefab) == entity)
        engine.destroy(prefab)
        assert(not engine.exists(prefab))
        for _, name in ipairs({"Camera", "MeshRenderer", "Light", "RigidBody", "Script", "AudioSource"}) do
            engine.remove_component(probe, name)
            assert(engine.get_component(probe, name) == nil)
        end
        engine.destroy(probe)
        assert(not engine.exists(probe))
        self.elapsed = 0
        for _, query in ipairs({engine.key_down, engine.key_pressed, engine.key_released}) do
            assert(type(query("Space")) == "boolean")
        end
        for _, query in ipairs({engine.mouse_down, engine.mouse_pressed, engine.mouse_released}) do
            assert(type(query("Left")) == "boolean")
        end
        for _, query in ipairs({engine.mouse_position, engine.mouse_delta, engine.mouse_wheel}) do
            local first, second = query()
            assert(type(first) == "number" and type(second) == "number")
        end
        assert(type(engine.input_focused()) == "boolean")
        local platform = engine.find("KinematicPlatform")
        if platform then
            assert(engine.get_component(platform, "RigidBody").Type == "Kinematic")
            assert(engine.get_component(engine.find("PointLight"), "Light").Type == "Point")
            assert(engine.get_component(engine.find("SpotLight"), "Light").Type == "Spot")
            assert(engine.get_component(engine.find("TriggerZone"), "RigidBody").IsTrigger)
        end
        engine.log("FeatureGallery: all 32 engine bindings exercised")
    end,

    OnUpdate = function(self, entity, dt)
        assert(dt > 0 and engine.exists(entity))
        self.elapsed = self.elapsed + dt
        local transform = engine.get_component(entity, "Transform")
        transform.Rotation[2] = self.elapsed * 0.25
        engine.set_component(entity, "Transform", transform)
        local platform = engine.find("KinematicPlatform")
        if platform then engine.set_position(platform, -4, 0.4 + math.sin(self.elapsed) * 0.15, 0) end
        local trigger = engine.find("TriggerZone")
        if trigger and self.elapsed > 1.5 and not self.triggerMoved then
            engine.set_position(trigger, 6, 1, 0)
            self.triggerMoved = true
        end
        -- The shipping runtime and editor play mode use the same input state.
        local sphere = engine.find("FallingSphere")
        if sphere and engine.input_focused() then
            local horizontal = (engine.key_down("Right") and 1 or 0) - (engine.key_down("Left") and 1 or 0)
            if horizontal ~= 0 then engine.apply_force(sphere, horizontal * 8, 0, 0) end
            if engine.key_pressed("Space") then engine.apply_impulse(sphere, 0, 4, 0) end
        end
    end,

    OnCollision = function(self, entity, other, began)
        assert(engine.exists(entity))
        engine.log(began and "FeatureGallery: contact began" or "FeatureGallery: contact ended")
    end,

    OnDestroy = function(self, entity)
        engine.log("FeatureGallery: destroyed")
    end
}
