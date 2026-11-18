#pragma once

#include "pc_media.h"

#include <string>

namespace godhand::pc
{
    class RigInspector
    {
    public:
        explicit RigInspector(const Scene &scene);
        ~RigInspector();
        RigInspector(const RigInspector &) = delete;
        RigInspector &operator=(const RigInspector &) = delete;
        void showWeights(Scene &scene);
        void showSkeleton() { m_skeleton = true; }
        void update(Scene &scene, Vector3 origin, float facing, float dt);
        void draw(const Scene &scene, Vector3 origin, float facing) const;

    private:
        void panel(const Scene &scene) const;
        void selectClip(int clip);
        void browseAnimations();
        void drawSkeleton(const Scene &scene, Vector3 origin, float facing) const;
        void control(Rectangle rect, const char *label, bool active = false, bool enabled = true) const;
        void label(const char *text, int x, int y, int size, Color color) const;
        float textWidth(const char *text, int size) const;

        Camera3D m_camera{};
        Font m_font{};
        std::string m_search;
        std::vector<int> m_filtered;
        std::optional<Scene::Vertex> m_selected;
        int m_clip = 0, m_frame = 0, m_offset = 0, m_joint = 20;
        float m_time = 0, m_orbit = 0.06f, m_scale = 2.40f;
        float m_scrollGrab = 0;
        bool m_weights = false, m_play = true, m_loop = true, m_bind = false, m_searching = false;
        bool m_skeleton = false, m_mesh = true, m_listDragging = false;
        const char *m_status = "";
    };
}
