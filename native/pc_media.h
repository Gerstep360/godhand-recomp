#pragma once

#include "raylib.h"

#include <filesystem>
#include <vector>

namespace godhand::pc
{
    class Scene
    {
    public:
        static Scene load(const std::filesystem::path &assets);
        ~Scene();
        Scene(const Scene &) = delete;
        Scene &operator=(const Scene &) = delete;

        void update(double seconds);
        void draw(Vector3 playerPosition, float facing) const;
        float floorHeight(Vector3 position) const;
        void pauseAnimation(bool paused) { m_animationPaused = paused; }
        void setMuted(bool muted);
        void play();
        float musicTime() const;

        int joints() const { return m_player.boneCount; }
        int poseCount() const { return m_clips[0].frameCount; }
        unsigned triangles() const;
        size_t stageParts() const { return m_room.size(); }
        unsigned audioChannels() const { return m_soundtrack.stream.channels; }
        int pose() const { return m_pose; }

    private:
        explicit Scene(const std::filesystem::path &assets);
        void release();
        void prepareMaterials(Model &model);

        struct Part
        {
            Model model{};
            std::vector<BoundingBox> meshBounds;
        };

        Model m_player{};
        ModelAnimation *m_clips = nullptr;
        int m_clipCount = 0;
        std::vector<Part> m_room;
        std::vector<Texture2D> m_textures;
        Shader m_surface{};
        Music m_soundtrack{};
        double m_animationTime = 0.0;
        int m_pose = -1;
        bool m_animationPaused = false;
    };
}
