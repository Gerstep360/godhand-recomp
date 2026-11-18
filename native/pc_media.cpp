#include "pc_media.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

namespace godhand::pc
{
    namespace
    {
        constexpr double kAnimationRate = 60.0; // raylib 5.5 samples glTF clips at 60 Hz.
        constexpr char kSurfaceShader[] = R"glsl(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main()
{
    vec4 surface = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (surface.a < 0.5)
        discard;
    finalColor = vec4(surface.rgb, 1.0);
}
)glsl";

        std::vector<std::filesystem::path> readRoom(const std::filesystem::path &root)
        {
            std::ifstream manifest(root / "scene.txt");
            std::string name;
            if (!std::getline(manifest, name) || name != "GH-PC-SCENE-1")
                throw std::runtime_error("Faltan los recursos PC. Ejecuta native/prepare_assets.py.");

            std::vector<std::filesystem::path> room;
            while (std::getline(manifest, name))
            {
                const std::filesystem::path file(name);
                if (file.empty() || file.has_parent_path() || file.has_root_path() || file.extension() != ".glb")
                    throw std::runtime_error("El indice contiene una ruta GLB invalida.");
                room.push_back(root / file);
            }
            if (room.empty())
                throw std::runtime_error("El indice no contiene geometria.");
            return room;
        }

        Model loadModel(const std::filesystem::path &path)
        {
            if (!std::filesystem::is_regular_file(path))
                throw std::runtime_error("Falta el modelo: " + path.string());
            Model model = LoadModel(path.string().c_str());
            if (!IsModelValid(model) || model.meshCount == 0)
            {
                UnloadModel(model);
                throw std::runtime_error("No se pudo cargar el modelo: " + path.string());
            }
            return model;
        }

        unsigned triangleCount(const Model &model)
        {
            unsigned count = 0;
            for (int i = 0; i < model.meshCount; ++i)
                count += static_cast<unsigned>(model.meshes[i].triangleCount);
            return count;
        }
    }

    Scene Scene::load(const std::filesystem::path &assets)
    {
        return Scene(assets);
    }

    Scene::Scene(const std::filesystem::path &assets)
    {
        const auto room = readRoom(assets);
        m_room.reserve(room.size());
        try
        {
            m_surface = LoadShaderFromMemory(nullptr, kSurfaceShader);
            if (!IsShaderValid(m_surface) || m_surface.id == rlGetShaderIdDefault())
                throw std::runtime_error("No se pudo compilar el shader de materiales de PC.");
            const auto player = assets / "player.glb";
            m_player = loadModel(player);
            prepareMaterials(m_player);
            m_clips = LoadModelAnimations(player.string().c_str(), &m_clipCount);
            if (!m_clips || m_clipCount < 1 || m_clips[0].frameCount < 2 || !IsModelAnimationValid(m_player, m_clips[0]))
                throw std::runtime_error("El GLB de Gene necesita una animacion compatible con su esqueleto.");

            for (const auto &part : room)
            {
                auto &entry = m_room.emplace_back();
                entry.model = loadModel(part);
                prepareMaterials(entry.model);
                entry.meshBounds.reserve(entry.model.meshCount);
                for (int i = 0; i < entry.model.meshCount; ++i)
                    entry.meshBounds.push_back(GetMeshBoundingBox(entry.model.meshes[i]));
            }
            const auto music = assets / "music.ogg";
            m_soundtrack = LoadMusicStream(music.string().c_str());
            if (!IsMusicValid(m_soundtrack) || m_soundtrack.stream.channels != 2u)
                throw std::runtime_error("La escena requiere una pista OGG estereo.");
            m_soundtrack.looping = true;
            update(0.0);
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    Scene::~Scene()
    {
        release();
    }

    void Scene::release()
    {
        if (IsMusicValid(m_soundtrack))
            UnloadMusicStream(m_soundtrack);
        if (m_clips)
            UnloadModelAnimations(m_clips, m_clipCount);
        for (auto &part : m_room)
            UnloadModel(part.model);
        if (m_player.meshes)
            UnloadModel(m_player);
        for (const auto &texture : m_textures)
            UnloadTexture(texture);
        if (m_surface.id && m_surface.id != rlGetShaderIdDefault())
            UnloadShader(m_surface);
    }

    void Scene::prepareMaterials(Model &model)
    {
        for (int i = 0; i < model.materialCount; ++i)
        {
            auto &material = model.materials[i];
            material.shader = m_surface;
            for (int map = MATERIAL_MAP_ALBEDO; map <= MATERIAL_MAP_BRDF; ++map)
            {
                auto &texture = material.maps[map].texture;
                if (texture.id == 0 || texture.id == rlGetTextureIdDefault())
                    continue;
                const auto loaded = std::find_if(m_textures.begin(), m_textures.end(),
                                                 [&](Texture2D entry) { return entry.id == texture.id; });
                if (loaded != m_textures.end())
                {
                    texture = *loaded;
                    continue;
                }
                GenTextureMipmaps(&texture);
                SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
                m_textures.push_back(texture);
            }
        }
    }

    void Scene::update(double seconds)
    {
        if (IsMusicStreamPlaying(m_soundtrack))
            UpdateMusicStream(m_soundtrack);
        if (m_animationPaused)
            return;
        const auto &clip = m_clips[0];
        m_animationTime = std::fmod(m_animationTime + seconds, clip.frameCount / kAnimationRate);
        const int next = static_cast<int>(m_animationTime * kAnimationRate);
        if (next != m_pose)
        {
            UpdateModelAnimation(m_player, clip, next);
            m_pose = next;
        }
    }

    void Scene::draw(Vector3 position, float facing) const
    {
        rlDisableBackfaceCulling();
        for (const auto &part : m_room)
            DrawModel(part.model, Vector3{}, 1.0f, WHITE);
        DrawModelEx(m_player, position, Vector3{0.0f, 1.0f, 0.0f}, facing, Vector3{1.0f, 1.0f, 1.0f}, WHITE);
        rlEnableBackfaceCulling();
    }

    float Scene::floorHeight(Vector3 position) const
    {
        const Ray ray{{position.x, position.y + 0.5f, position.z}, {0.0f, -1.0f, 0.0f}};
        float distance = 1.0f;
        float floor = position.y;
        for (const auto &part : m_room)
        {
            for (int i = 0; i < part.model.meshCount; ++i)
            {
                // Imported scene transforms are baked into the meshes.
                if (!GetRayCollisionBox(ray, part.meshBounds[i]).hit)
                    continue;
                const auto hit = GetRayCollisionMesh(ray, part.model.meshes[i], part.model.transform);
                if (hit.hit && hit.distance < distance && hit.normal.y > 0.6f)
                {
                    distance = hit.distance;
                    floor = hit.point.y;
                }
            }
        }
        return floor;
    }

    void Scene::setMuted(bool muted)
    {
        SetMusicVolume(m_soundtrack, muted ? 0.0f : 0.45f);
    }

    void Scene::play()
    {
        PlayMusicStream(m_soundtrack);
    }

    float Scene::musicTime() const
    {
        return GetMusicTimePlayed(m_soundtrack);
    }

    unsigned Scene::triangles() const
    {
        unsigned count = triangleCount(m_player);
        for (const auto &part : m_room)
            count += triangleCount(part.model);
        return count;
    }
}
