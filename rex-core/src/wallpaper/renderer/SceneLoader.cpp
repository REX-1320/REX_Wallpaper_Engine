#include "SceneLoader.hpp"
#include <shared/Logger.hpp>
#include <vector>
#include <filesystem>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace rex::wallpaper {

bool SceneLoader::Load(const std::string& path, ECS& ecs, RenderPipeline& pipeline) {
    if (!std::filesystem::exists(path)) {
        REX_LOG_WARN("SceneLoader", "File not found: {}", path);
        return LoadBuiltinScene(ecs, pipeline);
    }

    // Try glTF
    cgltf_options opts = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&opts, path.c_str(), &data);
    if (result != cgltf_result_success) {
        REX_LOG_ERROR("SceneLoader", "cgltf_parse_file failed: {}", static_cast<int>(result));
        return LoadBuiltinScene(ecs, pipeline);
    }

    result = cgltf_load_buffers(&opts, data, path.c_str());
    if (result != cgltf_result_success) {
        REX_LOG_ERROR("SceneLoader", "cgltf_load_buffers failed: {}", static_cast<int>(result));
        cgltf_free(data);
        return LoadBuiltinScene(ecs, pipeline);
    }

    cgltf_scene* scene = data->scene;
    if (!scene && data->scenes_count > 0)
        scene = &data->scenes[0];
    if (!scene) {
        REX_LOG_WARN("SceneLoader", "no scene found");
        cgltf_free(data);
        return LoadBuiltinScene(ecs, pipeline);
    }

    auto processNode = [&](cgltf_node* node, auto&& self) -> void {
        // Build transform
        XMFLOAT3 pos = {0,0,0}, scl = {1,1,1};
        if (node->has_translation) { pos.x = node->translation[0]; pos.y = node->translation[1]; pos.z = node->translation[2]; }
        if (node->has_scale) { scl.x = node->scale[0]; scl.y = node->scale[1]; scl.z = node->scale[2]; }

        if (node->mesh) {
            for (size_t p = 0; p < node->mesh->primitives_count; ++p) {
                auto& prim = node->mesh->primitives[p];
                if (!prim.indices || prim.type != cgltf_primitive_type_triangles) continue;

                const cgltf_accessor* posAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
                const cgltf_accessor* nrmAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* uvAcc  = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
                if (!posAcc || !prim.indices) continue;

                // Read vertices
                size_t vertCount = posAcc->count;
                std::vector<SceneVertex> verts(vertCount);
                for (size_t i = 0; i < vertCount; ++i) {
                    cgltf_accessor_read_float(posAcc, i, &verts[i].x, 3);
                    if (nrmAcc) cgltf_accessor_read_float(nrmAcc, i, &verts[i].nx, 3);
                    else { verts[i].nx = 0; verts[i].ny = 1; verts[i].nz = 0; }
                    if (uvAcc) cgltf_accessor_read_float(uvAcc, i, &verts[i].u, 2);
                    else { verts[i].u = 0; verts[i].v = 0; }
                }

                // Read indices
                size_t idxCount = prim.indices->count;
                std::vector<uint32_t> indices(idxCount);
                for (size_t i = 0; i < idxCount; ++i)
                    indices[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);

                uint32_t vbId = pipeline.CreateVertexBuffer(verts);
                uint32_t ibId = pipeline.CreateIndexBuffer(indices);

                Entity* entity = ecs.CreateEntity();
                entity->mesh = std::make_unique<MeshComponent>();
                entity->mesh->vertexBufferId = vbId;
                entity->mesh->indexBufferId = ibId;
                entity->mesh->indexCount = (uint32_t)idxCount;

                // Apply node transform
                XMFLOAT3 rot = {0,0,0};
                if (node->has_rotation) {
                    float qx=node->rotation[0], qy=node->rotation[1],
                          qz=node->rotation[2], qw=node->rotation[3];
                    float sr_cp = 2*(qw*qx + qy*qz);
                    float cr_cp = 1 - 2*(qx*qx + qy*qy);
                    rot.x = atan2f(sr_cp, cr_cp);
                    float sp = 2*(qw*qy - qz*qx);
                    rot.y = fabsf(sp) >= 1 ? copysignf(1.570796f, sp) : asinf(sp);
                    float sy_cp = 2*(qw*qz + qx*qy);
                    float cy_cp = 1 - 2*(qy*qy + qz*qz);
                    rot.z = atan2f(sy_cp, cy_cp);
                }
                entity->transform.position = pos;
                entity->transform.rotation = rot;
                entity->transform.scale = scl;
            }
        }

        // Add camera
        if (node->camera && node->camera->type == cgltf_camera_type_perspective) {
            Entity* camEnt = ecs.CreateEntity();
            camEnt->camera = std::make_unique<CameraComponent>();
            camEnt->camera->active = true;
            camEnt->camera->fov = node->camera->data.perspective.yfov;
            camEnt->camera->nearPlane = node->camera->data.perspective.znear;
            camEnt->camera->farPlane = node->camera->data.perspective.has_zfar
                ? node->camera->data.perspective.zfar : 1000.0f;
            camEnt->transform.position = pos;
            XMFLOAT3 rot = {0,0,0};
            if (node->has_rotation) {
                float qx=node->rotation[0], qy=node->rotation[1],
                      qz=node->rotation[2], qw=node->rotation[3];
                float sr_cp = 2*(qw*qx + qy*qz);
                float cr_cp = 1 - 2*(qx*qx + qy*qy);
                rot.x = atan2f(sr_cp, cr_cp);
                float sp = 2*(qw*qy - qz*qx);
                rot.y = fabsf(sp) >= 1 ? copysignf(1.570796f, sp) : asinf(sp);
                float sy_cp = 2*(qw*qz + qx*qy);
                float cy_cp = 1 - 2*(qy*qy + qz*qz);
                rot.z = atan2f(sy_cp, cy_cp);
            }
            camEnt->transform.rotation = rot;
        }

        for (size_t i = 0; i < node->children_count; ++i)
            self(node->children[i], self);
    };

    for (size_t i = 0; i < scene->nodes_count; ++i)
        processNode(scene->nodes[i], processNode);

    // Default camera
    if (!ecs.GetActiveCamera()) {
        Entity* camEnt = ecs.CreateEntity();
        camEnt->camera = std::make_unique<CameraComponent>();
        camEnt->camera->active = true;
        camEnt->transform.position = XMFLOAT3(0, 2, -5);
    }

    cgltf_free(data);
    REX_LOG_INFO("SceneLoader", "loaded {} entities", ecs.GetEntities().size());
    return true;
}

static const float CUBE_VERTICES[] = {
    // positions          normals          uvs
    -1,-1,-1, 0,0,-1, 0,1,
     1,-1,-1, 0,0,-1, 1,1,
    -1, 1,-1, 0,0,-1, 0,0,
     1, 1,-1, 0,0,-1, 1,0,
    -1,-1, 1, 0,0, 1, 0,1,
     1,-1, 1, 0,0, 1, 1,1,
    -1, 1, 1, 0,0, 1, 0,0,
     1, 1, 1, 0,0, 1, 1,0,
    -1,-1,-1,-1,0,0, 0,1,
    -1, 1,-1,-1,0,0, 0,0,
    -1,-1, 1,-1,0,0, 1,1,
    -1, 1, 1,-1,0,0, 1,0,
     1,-1,-1, 1,0,0, 0,1,
     1, 1,-1, 1,0,0, 0,0,
     1,-1, 1, 1,0,0, 1,1,
     1, 1, 1, 1,0,0, 1,0,
    -1,-1,-1, 0,-1,0, 0,1,
     1,-1,-1, 0,-1,0, 1,1,
    -1,-1, 1, 0,-1,0, 0,0,
     1,-1, 1, 0,-1,0, 1,0,
    -1, 1,-1, 0,1,0, 0,0,
     1, 1,-1, 0,1,0, 1,0,
    -1, 1, 1, 0,1,0, 0,1,
     1, 1, 1, 0,1,0, 1,1,
};

static const uint32_t CUBE_INDICES[] = {
    0,1,2, 1,3,2,
    4,6,5, 5,6,7,
    8,10,9, 9,10,11,
    12,13,14, 13,15,14,
    16,17,18, 17,19,18,
    20,22,21, 21,22,23,
};

bool SceneLoader::LoadBuiltinScene(ECS& ecs, RenderPipeline& pipeline) {
    REX_LOG_INFO("SceneLoader", "loading built-in cube");

    size_t vertCount = sizeof(CUBE_VERTICES) / (8 * sizeof(float));
    std::vector<SceneVertex> verts(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        verts[i].x  = CUBE_VERTICES[i*8+0];
        verts[i].y  = CUBE_VERTICES[i*8+1];
        verts[i].z  = CUBE_VERTICES[i*8+2];
        verts[i].nx = CUBE_VERTICES[i*8+3];
        verts[i].ny = CUBE_VERTICES[i*8+4];
        verts[i].nz = CUBE_VERTICES[i*8+5];
        verts[i].u  = CUBE_VERTICES[i*8+6];
        verts[i].v  = CUBE_VERTICES[i*8+7];
    }

    size_t idxCount = sizeof(CUBE_INDICES) / sizeof(uint32_t);
    std::vector<uint32_t> indices(CUBE_INDICES, CUBE_INDICES + idxCount);

    uint32_t vbId = pipeline.CreateVertexBuffer(verts);
    uint32_t ibId = pipeline.CreateIndexBuffer(indices);

    Entity* ent = ecs.CreateEntity();
    ent->mesh = std::make_unique<MeshComponent>();
    ent->mesh->vertexBufferId = vbId;
    ent->mesh->indexBufferId = ibId;
    ent->mesh->indexCount = (uint32_t)idxCount;

    // Default camera
    Entity* camEnt = ecs.CreateEntity();
    camEnt->camera = std::make_unique<CameraComponent>();
    camEnt->camera->active = true;
    camEnt->transform.position = XMFLOAT3(0, 2, -5);

    REX_LOG_INFO("SceneLoader", "built-in cube loaded ({} verts, {} indices)", vertCount, idxCount);
    return true;
}

} // namespace
