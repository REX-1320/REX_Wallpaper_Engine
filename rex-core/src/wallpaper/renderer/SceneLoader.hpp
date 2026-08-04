#pragma once
#include <string>
#include "ECS.hpp"
#include "RenderPipeline.hpp"

namespace rex::wallpaper {

class SceneLoader {
public:
    bool Load(const std::string& path, ECS& ecs, RenderPipeline& pipeline);

private:
    bool LoadBuiltinScene(ECS& ecs, RenderPipeline& pipeline);
};

} // namespace
