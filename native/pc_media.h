#pragma once

#include "raylib.h"
#include "pc_collision.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace godhand::pc
{
    class Scene
    {
    public:
        struct Vertex { int mesh, index; };
        struct Influence { unsigned char joint; float weight; };
        using Weights = std::array<Influence, 4>;

        static Scene load(const std::filesystem::path &assets);
        ~Scene();
        Scene(const Scene &) = delete;
        Scene &operator=(const Scene &) = delete;

        void update(double seconds, float speed = 0.0f, bool airborne = false);
        void draw(Vector3 playerPosition, float facing, bool weights = false, bool room = true) const;
        const CollisionWorld &collision() const { return m_collision; }
        void setMuted(bool muted);
        void play();
        float musicTime() const;
        void attack();
        bool striking() const { return m_striking; }
        Vector3 jointPosition(int index, Vector3 position, float facing) const;

        int clipCount() const { return m_clipCount; }
        const char *clipLabel(int index) const;
        int clipFrames(int index) const { return m_clips[index].frameCount; }
        void inspect(int clip, int frame, bool bind);
        void leaveInspection();
        const char *jointName(int joint) const;
        void heatWeights(int joint);
        std::optional<Vertex> pickVertex(Vector2 mouse, Camera3D camera, Vector3 position, float facing) const;
        Vector3 vertexPosition(Vertex vertex, Vector3 position, float facing) const;
        Weights weights(Vertex vertex) const;
        bool adjustWeight(Vertex vertex, int joint, float change);
        void resetWeights(Vertex vertex);
        bool saveWeights() const;

        int joints() const { return m_player.boneCount; }
        int jointParent(int joint) const { return m_player.bones[joint].parent; }
        int poseCount() const { return m_clips[m_idleClip].frameCount; }
        unsigned triangles() const;
        size_t stageParts() const { return m_room.size(); }
        unsigned audioChannels() const { return m_soundtrack.stream.channels; }
        int pose() const { return m_pose; }
        const char *clipName() const { return m_clips[m_activeClip].name; }

    private:
        explicit Scene(const std::filesystem::path &assets);
        void release();
        void prepareMaterials(Model &model);
        void applyPose(const ModelAnimation &clip, float airBlend = 0);
        void loadWeights();
        void setWeights(Vertex vertex, const Weights &weights);
        void editWeights(Vertex vertex, const Weights &weights);

        Model m_player{};
        ModelAnimation *m_clips = nullptr;
        int m_clipCount = 0;
        std::vector<Model> m_room;
        CollisionWorld m_collision;
        std::vector<Texture2D> m_textures;
        Shader m_surface{};
        int m_weightView = -1;
        Music m_soundtrack{};
        double m_animationTime = 0.0;
        double m_transition = 1.0;
        float m_airBlend = 0;
        std::vector<Transform> m_localPose, m_previousPose, m_worldPose;
        int m_idleClip = 0, m_walkClip = 0, m_runClip = 0;
        int m_strikeClip = -1;
        int m_activeClip = 0;
        int m_pose = -1;
        bool m_inspecting = false;
        bool m_striking = false;
        int m_heatJoint = -1;
        std::vector<std::vector<Weights>> m_originalWeights;
        std::filesystem::path m_weightFile;
        uint64_t m_weightSignature = 0;
    };
}
