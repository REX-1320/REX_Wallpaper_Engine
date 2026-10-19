#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>

namespace rex::wallpaper {

using namespace DirectX;

struct Transform {
    XMFLOAT3 position = {0,0,0};
    XMFLOAT3 rotation = {0,0,0};
    XMFLOAT3 scale    = {1,1,1};
    XMMATRIX GetMatrix() const {
        return XMMatrixScaling(scale.x, scale.y, scale.z) *
               XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
               XMMatrixTranslation(position.x, position.y, position.z);
    }
};

struct MeshComponent {
    uint32_t vertexBufferId;
    uint32_t indexBufferId;
    uint32_t indexCount;
    uint32_t materialId;
};

struct CameraComponent {
    float fov      = XM_PIDIV4;
    float nearPlane = 0.1f;
    float farPlane  = 1000.0f;
    bool active     = false;
};

struct LightComponent {
    XMFLOAT3 color     = {1,1,1};
    float    intensity = 1.0f;
    bool     castShadows = false;
};

struct Entity {
    uint32_t id;
    Transform transform;
    std::unique_ptr<MeshComponent> mesh;
    std::unique_ptr<CameraComponent> camera;
    std::unique_ptr<LightComponent> light;
};

class ECS {
public:
    Entity* CreateEntity() {
        m_entities.emplace_back();
        m_entities.back().id = m_nextId++;
        return &m_entities.back();
    }

    void RemoveEntity(uint32_t id) {
        std::erase_if(m_entities, [id](const Entity& e) { return e.id == id; });
    }

    std::vector<Entity>& GetEntities() { return m_entities; }
    const std::vector<Entity>& GetEntities() const { return m_entities; }

    Entity* FindEntity(uint32_t id) {
        for (auto& e : m_entities)
            if (e.id == id) return &e;
        return nullptr;
    }

    CameraComponent* GetActiveCamera() {
        for (auto& e : m_entities)
            if (e.camera && e.camera->active) return e.camera.get();
        return nullptr;
    }

private:
    std::vector<Entity> m_entities;
    uint32_t m_nextId = 1;
};

} // namespace
