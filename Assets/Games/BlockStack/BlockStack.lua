-- BlockStack: a complete small falling-block game made with Aster's public API.
-- Board seeds are authored empty entities named Seed:<column>:<row>.
local WIDTH, HEIGHT, TARGET = 10, 20, 2
local PREFAB = "Games/BlockStack/Block.prefab.json"
local SHAPES = {
    O = {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
    I = {{-1, 0}, {0, 0}, {1, 0}, {2, 0}},
    T = {{-1, 0}, {0, 0}, {1, 0}, {0, 1}},
    S = {{-1, 0}, {0, 0}, {0, 1}, {1, 1}},
    Z = {{-1, 1}, {0, 1}, {0, 0}, {1, 0}},
    J = {{-1, 1}, {-1, 0}, {0, 0}, {1, 0}},
    L = {{1, 1}, {-1, 0}, {0, 0}, {1, 0}}
}
local COLORS = {
    O = {1, 0.78, 0.10, 1}, I = {0.08, 0.80, 0.95, 1}, T = {0.65, 0.25, 0.9, 1},
    S = {0.22, 0.85, 0.4, 1}, Z = {0.95, 0.2, 0.3, 1}, J = {0.18, 0.35, 0.95, 1},
    L = {1, 0.45, 0.12, 1}, Seed = {0.38, 0.48, 0.58, 1}
}
local FONT = {
    A = {"010", "101", "111", "101", "101"}, C = {"111", "100", "100", "100", "111"},
    E = {"111", "100", "110", "100", "111"}, I = {"111", "010", "010", "010", "111"},
    L = {"100", "100", "100", "100", "111"}, N = {"101", "111", "111", "111", "101"},
    O = {"111", "101", "101", "101", "111"}, P = {"110", "101", "110", "100", "100"},
    R = {"110", "101", "110", "101", "101"}, S = {"111", "100", "111", "001", "111"},
    T = {"111", "010", "010", "010", "010"}, U = {"101", "101", "101", "101", "111"},
    V = {"101", "101", "101", "101", "010"}, W = {"101", "101", "111", "111", "101"},
    X = {"101", "101", "010", "101", "101"}, Y = {"101", "101", "010", "010", "010"},
    ["2"] = {"111", "001", "111", "100", "111"}
}
local DIGITS = {"1111110", "0110000", "1101101", "1111001", "0110011",
                "1011011", "1011111", "1110000", "1111111", "1111011"}
local SEGMENTS = {{0, 1, 0.62, 0.12}, {0.36, 0.5, 0.12, 0.8}, {0.36, -0.5, 0.12, 0.8},
                  {0, -1, 0.62, 0.12}, {-0.36, -0.5, 0.12, 0.8}, {-0.36, 0.5, 0.12, 0.8}, {0, 0, 0.62, 0.12}}

local function Remove(self, entity)
    if engine.exists(entity) then engine.destroy(entity) end
    self.nodes[entity] = nil
end

local function Cube(self, name, position, scale, color, prefab)
    local entity = prefab and engine.spawn_prefab(PREFAB, self.root) or engine.create(name)
    if not prefab then engine.set_parent(entity, self.root) end
    engine.set_name(entity, name)
    engine.set_component(entity, "Transform", {Translation = position, Scale = scale})
    engine.set_component(entity, "MeshRenderer", {
        Mesh = "Models/Cube.gltf", BaseColor = color, Metallic = 0.05, Roughness = 0.6, Visible = true
    })
    self.nodes[entity] = true
    return entity
end

local function Text(self, text, x, y, size, color)
    local entities = {}
    for index = 1, #text do
        local letter = FONT[text:sub(index, index)]
        if letter then
            for row = 1, 5 do
                for column = 1, 3 do
                    if letter[row]:sub(column, column) == "1" then
                        entities[#entities + 1] = Cube(self, "Display " .. text,
                            {x + (index - 1) * 4 * size + (column - 1) * size, y - (row - 1) * size, 0.4},
                            {size * 0.85, size * 0.85, 0.12}, color, false)
                    end
                end
            end
        end
    end
    return entities
end

local function Number(self, value, count, x, y, previous)
    if previous then for _, entity in ipairs(previous) do Remove(self, entity) end end
    local result = {}
    local text = string.format("%0" .. count .. "d", value):sub(-count)
    for index = 1, count do
        local pattern = DIGITS[tonumber(text:sub(index, index)) + 1]
        for segment, coordinates in ipairs(SEGMENTS) do
            if pattern:sub(segment, segment) == "1" then
                result[#result + 1] = Cube(self, "Display digit", {x + (index - 1) * 0.76 + coordinates[1] * 0.65,
                    y + coordinates[2] * 0.65, 0.45}, {coordinates[3] * 0.65, coordinates[4] * 0.65, 0.12},
                    {0.7, 0.95, 1, 1}, false)
            end
        end
    end
    return result
end

local function Publish(self)
    engine.set_name(self.root, string.format("BlockStack|status=%s|score=%d|lines=%d|piece=%s|x=%d|y=%d|rotation=%d",
        self.status, self.score, self.lines, self.kind or "O", self.x or 4, self.y or 18, self.rotation or 0))
    if self.displayScore ~= self.score then
        self.scoreDisplay = Number(self, self.score, 6, 6.7, 5.8, self.scoreDisplay)
        self.displayScore = self.score
    end
    if self.displayLines ~= self.lines then
        self.linesDisplay = Number(self, self.lines, 2, 7.2, 2.1, self.linesDisplay)
        self.displayLines = self.lines
    end
    if self.displayStatus ~= self.status then
        if self.statusDisplay then for _, entity in ipairs(self.statusDisplay) do Remove(self, entity) end end
        local label = ({playing = "PLAY", paused = "PAUSE", won = "WIN", lost = "OVER"})[self.status]
        local color = self.status == "lost" and {1, 0.2, 0.2, 1} or {0.3, 1, 0.55, 1}
        self.statusDisplay = Text(self, label, 6.7, -5.8, 0.20, color)
        self.displayStatus = self.status
    end
end

local function Points(kind, rotation)
    local points = {}
    for _, point in ipairs(SHAPES[kind]) do
        local x, y = point[1], point[2]
        if kind ~= "O" then
            for _ = 1, rotation % 4 do x, y = y, -x end
        end
        points[#points + 1] = {x, y}
    end
    return points
end

local function Fits(self, x, y, rotation)
    for _, point in ipairs(Points(self.kind, rotation)) do
        local column, row = x + point[1], y + point[2]
        if column < 0 or column >= WIDTH or row < 0 or row >= HEIGHT + 3 then return false end
        if row < HEIGHT and self.board[row][column] then return false end
    end
    return true
end

local function Visualize(self)
    local ghostY = self.y
    while Fits(self, self.x, ghostY - 1, self.rotation) do ghostY = ghostY - 1 end
    for index, point in ipairs(Points(self.kind, self.rotation)) do
        engine.set_position(self.active[index], self.x + point[1] - 4.5, self.y + point[2] - 9.5, 0)
        engine.set_position(self.ghost[index], self.x + point[1] - 4.5, ghostY + point[2] - 9.5, 0.5)
    end
    Publish(self)
end

local function TakeBag(self)
    if #self.bag == 0 then
        self.bag = {"I", "J", "L", "O", "S", "T", "Z"}
        for index = #self.bag, 2, -1 do
            self.seed = (self.seed * 48271) % 2147483647
            local other = 1 + self.seed % index
            self.bag[index], self.bag[other] = self.bag[other], self.bag[index]
        end
    end
    return table.remove(self.bag)
end

local function Spawn(self)
    self.kind, self.x, self.y, self.rotation = self.nextKind, 4, 18, 0
    self.nextKind = TakeBag(self)
    self.fallTime = 0
    self.active = {}
    for index = 1, 4 do
        self.active[index] = Cube(self, "Active:" .. index, {0, 0, 0}, {0.9, 0.9, 0.9}, COLORS[self.kind], true)
    end
    if self.preview then for _, entity in ipairs(self.preview) do Remove(self, entity) end end
    self.preview = {}
    for _, point in ipairs(Points(self.nextKind, 0)) do
        self.preview[#self.preview + 1] = Cube(self, "Next piece", {8 + point[1] * 0.6, -2.7 + point[2] * 0.6, 0},
            {0.52, 0.52, 0.52}, COLORS[self.nextKind], false)
    end
    if not Fits(self, self.x, self.y, self.rotation) then
        self.status = "lost"
        engine.log("BlockStack: game over")
    end
    Visualize(self)
end

local function ClearRows(self)
    local writeRow, cleared = 0, 0
    for readRow = 0, HEIGHT - 1 do
        local full = true
        for column = 0, WIDTH - 1 do if not self.board[readRow][column] then full = false end end
        if full then
            for column = 0, WIDTH - 1 do Remove(self, self.board[readRow][column].entity) end
            cleared = cleared + 1
        else
            self.board[writeRow] = self.board[readRow]
            for column, cell in pairs(self.board[writeRow]) do
                engine.set_name(cell.entity, "Block:" .. column .. ":" .. writeRow .. ":" .. cell.kind)
                engine.set_position(cell.entity, column - 4.5, writeRow - 9.5, 0)
            end
            writeRow = writeRow + 1
        end
    end
    for row = writeRow, HEIGHT - 1 do self.board[row] = {} end
    if cleared > 0 then
        self.lines = self.lines + cleared
        self.score = self.score + ({100, 300, 500, 800})[cleared]
        engine.play_audio(self.sound)
        engine.log("BlockStack: cleared " .. cleared .. " rows; score=" .. self.score)
    end
    if self.lines >= TARGET then
        self.status = "won"
        engine.log("BlockStack: victory")
    end
end

local function Lock(self)
    for _, point in ipairs(Points(self.kind, self.rotation)) do
        if self.y + point[2] >= HEIGHT then
            self.status = "lost"
            engine.log("BlockStack: game over")
            Publish(self)
            return
        end
    end
    for index, point in ipairs(Points(self.kind, self.rotation)) do
        local column, row = self.x + point[1], self.y + point[2]
        self.board[row][column] = {entity = self.active[index], kind = self.kind}
        engine.set_name(self.active[index], "Block:" .. column .. ":" .. row .. ":" .. self.kind)
    end
    self.active = {}
    ClearRows(self)
    if self.status == "playing" then Spawn(self) else Publish(self) end
end

local function Move(self, horizontal, vertical)
    if not Fits(self, self.x + horizontal, self.y + vertical, self.rotation) then return false end
    self.x, self.y = self.x + horizontal, self.y + vertical
    Visualize(self)
    return true
end

local function Rotate(self, direction)
    local rotation = (self.rotation + direction) % 4
    for _, kick in ipairs({{0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, 1}}) do
        if Fits(self, self.x + kick[1], self.y + kick[2], rotation) then
            self.x, self.y, self.rotation = self.x + kick[1], self.y + kick[2], rotation
            Visualize(self)
            return
        end
    end
end

local function Reset(self)
    for entity in pairs(self.nodes or {}) do if engine.exists(entity) then engine.destroy(entity) end end
    self.nodes, self.board, self.bag = {}, {}, {}
    self.score, self.lines, self.seed, self.nextKind = 0, 0, 1337, "O"
    self.status, self.repeatTime, self.repeatDirection = "playing", 0, 0
    self.displayScore, self.displayLines, self.displayStatus = nil, nil, nil
    self.scoreDisplay, self.linesDisplay, self.statusDisplay, self.preview = nil, nil, nil, nil
    for row = 0, HEIGHT - 1 do
        self.board[row] = {}
        for column = 0, WIDTH - 1 do
            if engine.find("Seed:" .. column .. ":" .. row) then
                self.board[row][column] = {kind = "Seed", entity = Cube(self, "Block:" .. column .. ":" .. row .. ":Seed",
                    {column - 4.5, row - 9.5, 0}, {0.9, 0.9, 0.9}, COLORS.Seed, true)}
            end
        end
    end
    self.ghost = {}
    for index = 1, 4 do
        self.ghost[index] = Cube(self, "Landing:" .. index, {0, 0, 0}, {0.22, 0.22, 0.15}, {0.45, 0.65, 0.7, 1}, false)
    end
    Text(self, "SCORE", 6.7, 8.3, 0.22, {0.7, 0.85, 1, 1})
    Text(self, "LINES", 6.7, 4.4, 0.22, {0.7, 0.85, 1, 1})
    Text(self, "NEXT", 6.7, -0.5, 0.22, {0.7, 0.85, 1, 1})
    Text(self, "CLEAR 2 LINES", -4.5, -11.0, 0.20, {0.55, 0.75, 0.9, 1})
    self.sound = engine.create("Line clear sound")
    engine.set_parent(self.sound, self.root)
    engine.set_component(self.sound, "AudioSource", {Path = "Audio/Feature.wav", Volume = 0.2, Pitch = 1.4, Spatial = false})
    self.nodes[self.sound] = true
    Spawn(self)
    engine.log("BlockStack: ready")
end

return {
    OnCreate = function(self, entity)
        self.root = entity
        Reset(self)
    end,
    OnUpdate = function(self, entity, dt)
        if engine.key_pressed("R") then Reset(self); return end
        if engine.key_pressed("P") and (self.status == "playing" or self.status == "paused") then
            self.status = self.status == "playing" and "paused" or "playing"
            Publish(self)
        end
        if self.status ~= "playing" or not engine.input_focused() then return end
        local left = engine.key_down("Left") or engine.key_down("A")
        local right = engine.key_down("Right") or engine.key_down("D")
        local direction = (right and 1 or 0) - (left and 1 or 0)
        if direction ~= self.repeatDirection then
            self.repeatDirection, self.repeatTime = direction, 0
            if direction ~= 0 then Move(self, direction, 0) end
        elseif direction ~= 0 then
            self.repeatTime = self.repeatTime + dt
            if self.repeatTime >= 0.18 then
                Move(self, direction, 0)
                self.repeatTime = self.repeatTime - 0.065
            end
        end
        if engine.key_pressed("Up") or engine.key_pressed("X") then Rotate(self, 1) end
        if engine.key_pressed("Z") then Rotate(self, -1) end
        if engine.key_pressed("Space") then
            while Move(self, 0, -1) do self.score = self.score + 2 end
            Lock(self)
            return
        end
        self.fallTime = self.fallTime + dt
        local interval = (engine.key_down("Down") or engine.key_down("S")) and 0.035 or 0.65
        while self.fallTime >= interval do
            self.fallTime = self.fallTime - interval
            if not Move(self, 0, -1) then Lock(self); return end
            if interval < 0.1 then self.score = self.score + 1 end
        end
        Publish(self)
    end,
    OnDestroy = function(self)
        for entity in pairs(self.nodes or {}) do if engine.exists(entity) then engine.destroy(entity) end end
        engine.log("BlockStack: stopped; score=" .. (self.score or 0) .. "; lines=" .. (self.lines or 0))
    end
}
