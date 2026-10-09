-- Listening fixture: the primary camera stays at the origin, looking along -Z.
-- Each stage lasts two simulation seconds; the ten-second sequence repeats.
-- Use stereo speakers/headphones and device audio for the physical listening gate.
local stages = {
    {name = "LEFT", position = {-2, 0, -2}, volume = 0.35},
    {name = "CENTER", position = {0, 0, -math.sqrt(8)}, volume = 0.35},
    {name = "RIGHT", position = {2, 0, -2}, volume = 0.35},
    {name = "FAR", position = {0, 0, -20}, volume = 0.35},
    {name = "SILENT", position = {0, 0, -math.sqrt(8)}, volume = 0}
}

local function applyStage(self, entity)
    local stage = stages[self.stage]
    engine.set_name(entity, "AudioValidation: " .. stage.name)
    engine.set_position(entity, table.unpack(stage.position))
    local source = engine.get_component(entity, "AudioSource")
    source.Volume = stage.volume
    engine.set_component(entity, "AudioSource", source)
    engine.log("AudioValidation: " .. stage.name)
end

return {
    OnCreate = function(self, entity)
        self.elapsed = 0
        self.stage = 1
        applyStage(self, entity)
    end,

    OnUpdate = function(self, entity, dt)
        self.elapsed = self.elapsed + dt
        -- Tolerate accumulated floating-point error at an exact two-second step.
        if self.elapsed + 1e-9 >= 2 then
            self.elapsed = math.max(0, self.elapsed - 2)
            self.stage = self.stage % #stages + 1
            applyStage(self, entity)
        end
    end
}
