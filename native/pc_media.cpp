#include "pc_media.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>

namespace godhand::pc
{
    namespace
    {
        constexpr double kAnimationStep = 0.017; // raylib 5.5 glTF sample interval.
        constexpr char kSurfaceShader[] = R"glsl(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform int weightView;
out vec4 finalColor;
void main()
{
    vec4 surface = texture(texture0, fragTexCoord) * colDiffuse;
    if (weightView == 0)
        surface *= fragColor;
    if (surface.a < 0.5)
        discard;
    vec3 rgb = weightView == 1 ? mix(fragColor.rgb, surface.rgb, 0.24) : surface.rgb;
    finalColor = vec4(rgb, 1.0);
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

        Transform interpolate(Transform a, Transform b, float fraction)
        {
            return {Vector3Lerp(a.translation, b.translation, fraction),
                    QuaternionSlerp(a.rotation, b.rotation, fraction),
                    Vector3Lerp(a.scale, b.scale, fraction)};
        }

        Vector3 playerToWorld(Vector3 point, Vector3 position, float facing)
        {
            return Vector3Add(position, Vector3Transform(point, MatrixRotateY(facing * DEG2RAD)));
        }

        void localizeAnimation(ModelAnimation &clip)
        {
            for (int f = 0; f < clip.frameCount; ++f)
            {
                auto *pose = clip.framePoses[f];
                for (int bone = clip.boneCount - 1; bone >= 0; --bone)
                {
                    const int index = clip.bones[bone].parent;
                    if (index < 0)
                        continue;
                    const auto &parent = pose[index];
                    auto &child = pose[bone];
                    const auto inverse = QuaternionInvert(parent.rotation);
                    child.translation = Vector3Divide(Vector3RotateByQuaternion(Vector3Subtract(child.translation, parent.translation), inverse), parent.scale);
                    child.rotation = QuaternionMultiply(inverse, child.rotation);
                    child.scale = Vector3Divide(child.scale, parent.scale);
                }
            }
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
            m_weightView = GetShaderLocation(m_surface, "weightView");
            const auto player = assets / "player.glb";
            m_player = loadModel(player);
            prepareMaterials(m_player);
            m_clips = LoadModelAnimations(player.string().c_str(), &m_clipCount);
            if (!m_clips || m_clipCount < 1)
                throw std::runtime_error("El GLB de Gene necesita una animacion compatible con su esqueleto.");
            for (int bone = 0; bone < m_player.boneCount; ++bone)
                if (m_player.bones[bone].parent >= bone)
                    throw std::runtime_error("El GLB necesita huesos ordenados de padre a hijo.");
            for (int i = 0; i < m_clipCount; ++i)
            {
                auto &clip = m_clips[i];
                if (!IsModelAnimationValid(m_player, clip) || clip.frameCount < 2)
                    throw std::runtime_error("Hay una animacion incompatible con el esqueleto de Gene.");
                localizeAnimation(clip);
                const std::string_view name(clip.name);
                if (name == "idle") m_idleClip = i;
                if (name == "walk") m_walkClip = i;
                if (name == "run") m_runClip = i;
                if (name == "strike") m_strikeClip = i;
            }
            m_activeClip = m_idleClip;
            m_localPose.resize(m_player.boneCount);
            m_previousPose.resize(m_player.boneCount);
            m_worldPose.resize(m_player.boneCount);
            m_weightFile = assets / "gene-weights.txt";
            loadWeights();

            for (const auto &part : room)
            {
                auto &entry = m_room.emplace_back(loadModel(part));
                prepareMaterials(entry);
                for (int i = 0; i < entry.meshCount; ++i)
                    m_collision.add(entry.meshes[i], entry.transform);
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
            UnloadModel(part);
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

    void Scene::update(double seconds, float speed, bool airborne)
    {
        if (IsMusicStreamPlaying(m_soundtrack))
            UpdateMusicStream(m_soundtrack);
        if (m_inspecting)
            return;
        if (m_striking && m_activeClip == m_strikeClip &&
            m_animationTime >= (m_clips[m_strikeClip].frameCount - 1) * kAnimationStep)
            m_striking = false;
        const int requested = m_striking ? m_strikeClip :
                              (speed < 0.14f ? m_idleClip : (speed > 4.5f ? m_runClip : m_walkClip));
        if (requested != m_activeClip)
        {
            const double phase = m_animationTime / (m_clips[m_activeClip].frameCount * kAnimationStep);
            m_previousPose = m_localPose;
            m_animationTime = (speed >= 0.14f && m_activeClip != m_idleClip && m_activeClip != m_strikeClip && !m_striking) ?
                              phase * m_clips[requested].frameCount * kAnimationStep : 0.0;
            m_activeClip = requested;
            m_transition = 0.0;
        }
        const auto &clip = m_clips[m_activeClip];
        // Distances from the MOT root tracks over each locomotion cycle.
        const double authoredSpeed = requested == m_runClip && m_runClip != m_idleClip ? 15.32 : 3.48;
        const double rate = m_striking || speed < 0.14f ? 1.0 : std::clamp(speed / authoredSpeed, 0.18, 1.4);
        m_animationTime = m_striking ? std::min(m_animationTime + seconds, (clip.frameCount - 1) * kAnimationStep)
                                     : std::fmod(m_animationTime + seconds * rate, clip.frameCount * kAnimationStep);
        m_transition = std::min(m_transition + seconds / (m_striking ? 0.05 : 0.15), 1.0);
        const double frame = m_animationTime / kAnimationStep;
        m_pose = static_cast<int>(frame);
        const int next = m_striking ? std::min(m_pose + 1, clip.frameCount - 1) : (m_pose + 1) % clip.frameCount;
        const float fraction = static_cast<float>(frame - m_pose);
        const float blend = static_cast<float>(m_transition * m_transition * (3 - 2 * m_transition));
        m_airBlend += ((airborne ? 1.0f : 0.0f) - m_airBlend) * (1.0f - std::exp(-18.0f * static_cast<float>(seconds)));
        for (int bone = 0; bone < clip.boneCount; ++bone)
        {
            const auto sample = interpolate(clip.framePoses[m_pose][bone], clip.framePoses[next][bone], fraction);
            auto &local = m_localPose[bone];
            local = m_transition < 1.0 ? interpolate(m_previousPose[bone], sample, blend) : sample;
        }
        applyPose(clip, m_airBlend);
    }

    void Scene::applyPose(const ModelAnimation &clip, float airBlend)
    {
        // Hips, knees and ankles bend while the physics controller owns height.
        constexpr int firstLegJoint = 19;
        constexpr std::array bend{-.38f, .53f, -.18f, 0.0f, -.23f, .44f, -.14f};
        for (int bone = 0; bone < clip.boneCount; ++bone)
        {
            auto &world = m_worldPose[bone];
            auto local = m_localPose[bone];
            if (airBlend > .0001f && bone >= firstLegJoint && bone < firstLegJoint + static_cast<int>(bend.size()))
                local.rotation = QuaternionMultiply(local.rotation,
                    QuaternionFromAxisAngle({1, 0, 0}, bend[bone - firstLegJoint] * airBlend));
            world = local;
            const int index = clip.bones[bone].parent;
            if (index >= 0)
            {
                const auto &parent = m_worldPose[index];
                world.translation = Vector3Add(parent.translation, Vector3RotateByQuaternion(Vector3Multiply(local.translation, parent.scale), parent.rotation));
                world.rotation = QuaternionMultiply(parent.rotation, local.rotation);
                world.scale = Vector3Multiply(parent.scale, local.scale);
            }
        }
        Transform *pose = m_worldPose.data();
        ModelAnimation current = clip;
        current.frameCount = 1;
        current.framePoses = &pose;
        UpdateModelAnimation(m_player, current, 0);
    }

    void Scene::draw(Vector3 position, float facing, bool weights, bool room) const
    {
        rlDisableBackfaceCulling();
        if (room)
            for (const auto &part : m_room)
                DrawModel(part, Vector3{}, 1.0f, WHITE);
        const int playerColors = weights ? 1 : (m_heatJoint >= 0 ? 2 : 0);
        if (playerColors)
            SetShaderValue(m_surface, m_weightView, &playerColors, SHADER_UNIFORM_INT);
        DrawModelEx(m_player, position, Vector3{0.0f, 1.0f, 0.0f}, facing, Vector3{1.0f, 1.0f, 1.0f}, WHITE);
        if (playerColors)
        {
            const int regular = 0;
            SetShaderValue(m_surface, m_weightView, &regular, SHADER_UNIFORM_INT);
        }
        rlEnableBackfaceCulling();
    }

    void Scene::attack()
    {
        if (m_strikeClip >= 0 && !m_inspecting)
            m_striking = true;
    }

    Vector3 Scene::jointPosition(int index, Vector3 position, float facing) const
    {
        if (index < 0 || index >= m_player.boneCount)
            return position;
        return playerToWorld(m_worldPose[index].translation, position, facing);
    }

    const char *Scene::clipLabel(int index) const
    {
        if (index == m_idleClip) return "MOT_009  Guardia";
        if (index == m_walkClip) return "MOT_021  Caminar";
        if (index == m_runClip) return "MOT_025  Correr";
        if (index == m_strikeClip) return "MOT_032  Golpe";
        return m_clips[index].name;
    }

    void Scene::inspect(int clip, int frame, bool bind)
    {
        if (m_inspecting && m_activeClip == clip && m_pose == (bind ? -1 : frame))
            return;
        m_inspecting = true;
        m_striking = false;
        m_activeClip = clip;
        m_pose = bind ? -1 : frame;
        if (bind)
        {
            std::copy_n(m_player.bindPose, m_player.boneCount, m_worldPose.begin());
            Transform *pose = m_worldPose.data();
            ModelAnimation current = m_clips[clip];
            current.frameCount = 1;
            current.framePoses = &pose;
            UpdateModelAnimation(m_player, current, 0);
        }
        else
        {
            std::copy_n(m_clips[clip].framePoses[frame], m_player.boneCount, m_localPose.begin());
            applyPose(m_clips[clip]);
        }
    }

    void Scene::leaveInspection()
    {
        m_inspecting = false;
        m_activeClip = m_idleClip;
        m_animationTime = 0;
        m_transition = 1;
        m_pose = -1;
    }

    const char *Scene::jointName(int joint) const
    {
        switch (joint)
        {
            case 1: return "Cintura";
            case 18: return "Pelvis";
            case 19: return "Muslo derecho";
            case 20: return "Rodilla derecha";
            case 21: return "Tobillo derecho";
            case 22: return "Punta derecha";
            case 23: return "Muslo izquierdo";
            case 24: return "Rodilla izquierda";
            case 25: return "Tobillo izquierdo";
            case 26: return "Punta izquierda";
            default: return m_player.bones[joint].name;
        }
    }

    Scene::Weights Scene::weights(Vertex vertex) const
    {
        const auto &mesh = m_player.meshes[vertex.mesh];
        Weights result;
        for (int i = 0; i < 4; ++i)
            result[i] = {mesh.boneIds[vertex.index * 4 + i], mesh.boneWeights[vertex.index * 4 + i]};
        return result;
    }

    void Scene::setWeights(Vertex vertex, const Weights &value)
    {
        auto &mesh = m_player.meshes[vertex.mesh];
        for (int i = 0; i < 4; ++i)
        {
            mesh.boneIds[vertex.index * 4 + i] = value[i].joint;
            mesh.boneWeights[vertex.index * 4 + i] = value[i].weight;
        }
    }

    Vector3 Scene::vertexPosition(Vertex vertex, Vector3 position, float facing) const
    {
        const auto &mesh = m_player.meshes[vertex.mesh];
        const float *v = (mesh.animVertices ? mesh.animVertices : mesh.vertices) + vertex.index * 3;
        return playerToWorld({v[0], v[1], v[2]}, position, facing);
    }

    std::optional<Scene::Vertex> Scene::pickVertex(Vector2 mouse, Camera3D camera, Vector3 position, float facing) const
    {
        const Ray ray = GetScreenToWorldRay(mouse, camera);
        const Matrix model = MatrixMultiply(MatrixRotateY(facing * DEG2RAD), MatrixTranslate(position.x, position.y, position.z));
        float surfaceDistance = std::numeric_limits<float>::max();
        for (int m = 0; m < m_player.meshCount; ++m)
        {
            Mesh posed = m_player.meshes[m];
            if (posed.animVertices)
                posed.vertices = posed.animVertices;
            const auto hit = GetRayCollisionMesh(ray, posed, model);
            if (hit.hit)
                surfaceDistance = std::min(surfaceDistance, hit.distance);
        }
        float best = 18.0f;
        std::optional<Vertex> result;
        for (int m = 0; m < m_player.meshCount; ++m)
            for (int i = 0; i < static_cast<int>(m_originalWeights[m].size()); ++i)
            {
                const auto point = vertexPosition({m, i}, position, facing);
                const float depth = Vector3DotProduct(Vector3Subtract(point, ray.position), ray.direction);
                if (depth > surfaceDistance + 0.10f || depth <= 0.0f)
                    continue;
                const auto screen = GetWorldToScreen(point, camera);
                const float score = Vector2Distance(mouse, screen) + 0.7f * depth;
                if (score < best)
                {
                    best = score;
                    result = Vertex{m, i};
                }
            }
        return result;
    }

    void Scene::heatWeights(int joint)
    {
        m_heatJoint = joint;
        for (int m = 0; m < m_player.meshCount; ++m)
        {
            auto &mesh = m_player.meshes[m];
            if (m_originalWeights[m].empty())
                continue;
            std::vector<Color> colors(mesh.vertexCount);
            for (int v = 0; v < mesh.vertexCount; ++v)
            {
                float value = 0;
                for (const auto influence : weights({m, v}))
                    if (influence.joint == joint)
                        value += influence.weight;
                colors[v] = ColorLerp(Color{49, 67, 90, 255}, Color{255, 143, 66, 255}, std::clamp(value, 0.0f, 1.0f));
            }
            constexpr int attribute = RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR;
            const int bytes = static_cast<int>(colors.size() * sizeof(Color));
            if (mesh.vboId[attribute])
            {
                UpdateMeshBuffer(mesh, attribute, colors.data(), bytes, 0);
                continue;
            }
            rlEnableVertexArray(mesh.vaoId);
            mesh.vboId[attribute] = rlLoadVertexBuffer(colors.data(), bytes, true);
            rlSetVertexAttribute(attribute, 4, RL_UNSIGNED_BYTE, true, 0, 0);
            rlEnableVertexAttribute(attribute);
            rlDisableVertexBuffer();
            rlDisableVertexArray();
        }
    }

    bool Scene::adjustWeight(Vertex vertex, int joint, float change)
    {
        Weights edited = weights(vertex);
        auto slot = std::find_if(edited.begin(), edited.end(), [joint](Influence entry) { return entry.joint == joint; });
        if (slot == edited.end())
        {
            if (change < 0)
                return false;
            slot = std::min_element(edited.begin(), edited.end(), [](Influence a, Influence b) { return a.weight < b.weight; });
            *slot = {static_cast<unsigned char>(joint), 0};
        }
        const float value = std::clamp(slot->weight + change, 0.0f, 1.0f);
        if (value == slot->weight)
            return false;
        float remaining = 0;
        for (const auto &entry : edited)
            if (&entry != &*slot)
                remaining += entry.weight;
        if (remaining <= 1e-6f)
        {
            if (value < 1)
                return false;
            remaining = 1;
        }
        for (auto &entry : edited)
            if (&entry != &*slot)
                entry.weight *= (1 - value) / remaining;
        slot->weight = value;
        editWeights(vertex, edited);
        return true;
    }

    void Scene::resetWeights(Vertex vertex)
    {
        editWeights(vertex, m_originalWeights[vertex.mesh][vertex.index]);
    }

    void Scene::editWeights(Vertex vertex, const Weights &value)
    {
        const auto &original = m_originalWeights[vertex.mesh][vertex.index];
        const float *p = m_player.meshes[vertex.mesh].vertices + vertex.index * 3;
        for (int m = 0; m < m_player.meshCount; ++m)
            for (int i = 0; i < static_cast<int>(m_originalWeights[m].size()); ++i)
            {
                const float *q = m_player.meshes[m].vertices + i * 3;
                const auto same = [](Influence a, Influence b) {
                    return a.joint == b.joint && std::abs(a.weight - b.weight) < 1e-6f;
                };
                if (std::equal(p, p + 3, q) &&
                    std::equal(original.begin(), original.end(), m_originalWeights[m][i].begin(), same))
                    setWeights({m, i}, value);
            }
        if (m_heatJoint >= 0)
            heatWeights(m_heatJoint);
        m_inspecting = false; // Reapply the inspected pose with the new weights.
    }

    void Scene::loadWeights()
    {
        m_originalWeights.resize(m_player.meshCount);
        m_weightSignature = 14695981039346656037ULL;
        const auto hash = [this](uint32_t value) {
            m_weightSignature = (m_weightSignature ^ value) * 1099511628211ULL;
        };
        for (int m = 0; m < m_player.meshCount; ++m)
        {
            const auto &mesh = m_player.meshes[m];
            if (!mesh.boneWeights || !mesh.boneIds)
                continue;
            hash(mesh.vertexCount);
            auto &original = m_originalWeights[m];
            original.reserve(mesh.vertexCount);
            for (int v = 0; v < mesh.vertexCount; ++v)
            {
                original.push_back(weights({m, v}));
                for (int axis = 0; axis < 3; ++axis)
                    hash(std::bit_cast<uint32_t>(mesh.vertices[v * 3 + axis]));
                for (const auto influence : original.back())
                {
                    hash(influence.joint);
                    hash(std::bit_cast<uint32_t>(influence.weight));
                }
            }
        }
        std::ifstream file(m_weightFile);
        file.imbue(std::locale::classic());
        if (!file)
            return;
        std::string format;
        uint64_t signature = 0;
        file >> format >> signature;
        if (format != "GH-GENE-WEIGHTS-1" || signature != m_weightSignature)
        {
            TraceLog(LOG_WARNING, "Los ajustes de pesos pertenecen a otra malla.");
            return;
        }
        std::vector<std::pair<Vertex, Weights>> pending;
        for (file >> std::ws; file.peek() != EOF; file >> std::ws)
        {
            Vertex v{};
            if (!(file >> v.mesh >> v.index) || v.mesh < 0 || v.mesh >= m_player.meshCount ||
                v.index < 0 || v.index >= static_cast<int>(m_originalWeights[v.mesh].size()))
                return;
            Weights value;
            float sum = 0;
            for (auto &influence : value)
            {
                int joint = 0;
                if (!(file >> joint >> influence.weight) || joint < 0 || joint >= m_player.boneCount ||
                    !std::isfinite(influence.weight) || influence.weight < 0 || influence.weight > 1)
                    return;
                influence.joint = static_cast<unsigned char>(joint);
                sum += influence.weight;
            }
            if (std::abs(sum - 1.0f) > 1e-5f)
                return;
            pending.emplace_back(v, value);
        }
        for (const auto &[vertex, value] : pending)
            setWeights(vertex, value);
    }

    bool Scene::saveWeights() const
    {
        auto temp = m_weightFile;
        temp += ".tmp";
        std::ofstream file(temp, std::ios::trunc);
        file.imbue(std::locale::classic());
        file.precision(std::numeric_limits<float>::max_digits10);
        file << "GH-GENE-WEIGHTS-1 " << m_weightSignature << '\n';
        for (int m = 0; m < m_player.meshCount; ++m)
            for (int v = 0; v < static_cast<int>(m_originalWeights[m].size()); ++v)
            {
                const auto value = weights({m, v});
                const auto &original = m_originalWeights[m][v];
                const auto same = [](Influence a, Influence b) { return a.joint == b.joint && a.weight == b.weight; };
                if (std::equal(value.begin(), value.end(), original.begin(), same))
                    continue;
                file << m << ' ' << v;
                for (const auto entry : value)
                    file << ' ' << static_cast<int>(entry.joint) << ' ' << entry.weight;
                file << '\n';
            }
        file.close();
        if (!file)
            return false;
        std::error_code error;
        std::filesystem::rename(temp, m_weightFile, error);
        return !error;
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
            count += triangleCount(part);
        return count;
    }
}
