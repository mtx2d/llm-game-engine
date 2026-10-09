return {
    OnCreate = function(self, entity)
        local body = engine.get_component(entity, "RigidBody")
        assert(body.Shape == "Capsule" and body.IsTrigger and body.Type == "Static")
        self.contacts = 0
    end,
    OnCollision = function(self, entity, other, began)
        assert(engine.exists(entity))
        if began then
            assert(engine.exists(other))
            assert(engine.get_component(other, "RigidBody") ~= nil)
            self.contacts = self.contacts + 1
            engine.log("FeatureGallery: capsule trigger entered")
        else
            self.contacts = self.contacts - 1
            assert(self.contacts >= 0)
            engine.log("FeatureGallery: capsule trigger exited")
        end
    end,
    OnDestroy = function(self)
        engine.log("FeatureGallery: trigger lifecycle released")
    end
}
