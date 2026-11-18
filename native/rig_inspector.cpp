#include "rig_inspector.h"
#include "raymath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace godhand::pc
{
    namespace
    {
        constexpr int kWidth = 342;
        constexpr float kFrameStep = .017f;
        constexpr Color kBackdrop{24, 26, 30, 255};
        constexpr Color kPanel{24, 23, 22, 255};
        constexpr Color kControl{46, 43, 40, 255};
        constexpr Color kInk{239, 232, 220, 255};
        constexpr Color kMuted{187, 178, 164, 255};
        constexpr Color kCopper{241, 161, 100, 255};
        constexpr Rectangle kAnimations{20, 65, 146, 34};
        constexpr Rectangle kWeights{176, 65, 146, 34};
        constexpr Rectangle kSearch{20, 113, 302, 34};
        constexpr Rectangle kPreviousJoint{20, 135, 37, 34};
        constexpr Rectangle kNextJoint{285, 135, 37, 34};
        constexpr Rectangle kLess{20, 391, 146, 34};
        constexpr Rectangle kMore{176, 391, 146, 34};
        constexpr Rectangle kRestore{20, 435, 146, 34};
        constexpr Rectangle kSave{176, 435, 146, 34};

        int rows() { return std::max(3, (GetScreenHeight() - 274) / 30); }
        Rectangle row(int i) { return {20, 160.0f + i * 30, 280, 27}; }
        Rectangle scrollbar() { return {307, 160, 15, rows() * 30.0f - 3}; }
        Rectangle scrollThumb(Rectangle track, int total, int offset)
        {
            const float height = std::max(25.0f, track.height * rows() / total);
            const float progress = static_cast<float>(offset) / (total - rows());
            return {track.x, track.y + (track.height - height) * progress, track.width, height};
        }
        float timelineY() { return static_cast<float>(GetScreenHeight() - 98); }
        Rectangle playback() { return {20, timelineY(), 98, 34}; }
        Rectangle loop() { return {123, timelineY(), 91, 34}; }
        Rectangle bindPose() { return {219, timelineY(), 103, 34}; }
        Rectangle timeline() { return {20, timelineY() + 43, 302, 15}; }
        Rectangle skeletonButton() { return {GetScreenWidth() - 312.0f, 20, 119, 34}; }
        Rectangle meshButton() { return {GetScreenWidth() - 185.0f, 20, 81, 34}; }
        Rectangle zoomOut() { return {GetScreenWidth() - 96.0f, 20, 34, 34}; }
        Rectangle zoomIn() { return {GetScreenWidth() - 54.0f, 20, 34, 34}; }

        bool hovered(Rectangle rect) { return CheckCollisionPointRec(GetMousePosition(), rect); }
        bool clicked(Rectangle rect) { return hovered(rect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }
        bool keyStep(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }
    }

    RigInspector::RigInspector(const Scene &scene)
    {
        const auto font = std::filesystem::path(GetApplicationDirectory()) / "Roboto-Regular.ttf";
        m_font = GetFontDefault();
        if (FileExists(font.string().c_str()))
        {
            std::array<int, 224> codes{};
            for (int i = 0; i < static_cast<int>(codes.size()); ++i)
                codes[i] = i + 32;
            m_font = LoadFontEx(font.string().c_str(), 32, codes.data(), static_cast<int>(codes.size()));
            SetTextureFilter(m_font.texture, TEXTURE_FILTER_BILINEAR);
        }
        for (int i = 0; i < scene.clipCount(); ++i)
            if (std::string_view(scene.clipLabel(i)).starts_with("MOT_009"))
                m_clip = i;
        m_offset = std::max(0, m_clip - 2);
    }

    RigInspector::~RigInspector()
    {
        if (m_font.texture.id != GetFontDefault().texture.id)
            UnloadFont(m_font);
    }

    void RigInspector::label(const char *value, int x, int y, int size, Color color) const
    {
        DrawTextEx(m_font, value, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size), .5f, color);
    }

    float RigInspector::textWidth(const char *value, int size) const
    {
        return MeasureTextEx(m_font, value, static_cast<float>(size), .5f).x;
    }

    void RigInspector::control(Rectangle rect, const char *value, bool active, bool enabled) const
    {
        const Color background = active ? kCopper : enabled && hovered(rect) ? Color{68, 59, 51, 255} : kControl;
        DrawRectangleRec(rect, background);
        constexpr int size = 16;
        label(value, static_cast<int>(rect.x + (rect.width - textWidth(value, size)) / 2),
              static_cast<int>(rect.y + (rect.height - size) / 2), size,
              active ? kPanel : enabled ? kInk : kMuted);
    }

    void RigInspector::showWeights(Scene &scene)
    {
        m_weights = true;
        m_play = false;
        m_searching = false;
        scene.heatWeights(m_joint);
    }

    void RigInspector::selectClip(int clip)
    {
        m_clip = clip;
        m_frame = 0;
        m_time = 0;
        m_bind = false;
    }

    void RigInspector::browseAnimations()
    {
        const int count = static_cast<int>(m_filtered.size());
        const int maxOffset = std::max(0, count - rows());
        const auto found = std::find(m_filtered.begin(), m_filtered.end(), m_clip);
        const int current = static_cast<int>(found - m_filtered.begin());
        const int step = keyStep(KEY_DOWN) - keyStep(KEY_UP) +
                         rows() * (keyStep(KEY_PAGE_DOWN) - keyStep(KEY_PAGE_UP));
        if (count && (step || IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_END)))
        {
            m_searching = false;
            int selection = current == count ? (step < 0 ? count - 1 : 0) : current + step;
            if (IsKeyPressed(KEY_HOME)) selection = 0;
            if (IsKeyPressed(KEY_END)) selection = count - 1;
            selection = std::clamp(selection, 0, count - 1);
            if (m_clip != m_filtered[selection])
                selectClip(m_filtered[selection]);
            m_offset = std::clamp(m_offset, selection - rows() + 1, selection);
        }

        const auto mouse = GetMousePosition();
        if (mouse.x < kWidth)
            m_offset -= static_cast<int>(GetMouseWheelMove() * 3);
        m_offset = std::clamp(m_offset, 0, maxOffset);
        const auto track = scrollbar();
        if (!maxOffset || !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            m_listDragging = false;
        if (maxOffset && clicked(track))
        {
            const auto thumb = scrollThumb(track, count, m_offset);
            m_scrollGrab = hovered(thumb) ? mouse.y - thumb.y : thumb.height * .5f;
            m_listDragging = true;
        }
        if (m_listDragging)
        {
            const auto thumb = scrollThumb(track, count, m_offset);
            const float progress = (mouse.y - track.y - m_scrollGrab) / (track.height - thumb.height);
            m_offset = static_cast<int>(std::lround(std::clamp(progress, 0.0f, 1.0f) * maxOffset));
        }
        for (int i = 0; i < rows() && i + m_offset < count; ++i)
            if (!m_listDragging && clicked(row(i)))
                selectClip(m_filtered[i + m_offset]);
    }

    void RigInspector::update(Scene &scene, Vector3 origin, float facing, float dt)
    {
        if (clicked(kAnimations)) m_weights = false;
        if (clicked(kWeights)) showWeights(scene);
        const auto mouse = GetMousePosition();
        if (clicked(skeletonButton()))
        {
            m_skeleton = !m_skeleton;
            if (!m_skeleton) m_mesh = true;
        }
        if (clicked(meshButton()))
        {
            m_mesh = !m_mesh;
            if (!m_mesh) m_skeleton = true;
        }
        const int zoom = clicked(zoomOut()) - clicked(zoomIn());
        m_scale = std::clamp(m_scale + zoom * .18f, 1.35f, 3.6f);
        if (mouse.x > kWidth)
        {
            m_scale = std::clamp(m_scale - GetMouseWheelMove() * .16f, 1.35f, 3.6f);
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
                m_orbit += GetMouseDelta().x * .007f;
        }
        if (!m_searching)
            m_orbit += (IsKeyDown(KEY_D) - IsKeyDown(KEY_A)) * dt * 1.8f;
        m_orbit = std::remainder(m_orbit, 2 * PI);
        const float angle = facing * DEG2RAD + m_orbit;
        const Vector3 view{std::sin(angle), 0.08f, std::cos(angle)};
        const Vector3 screenRight{view.z, 0, -view.x};
        const auto center = Vector3Add(origin, {0, .95f, 0});
        const auto offset = Vector3Scale(screenRight, -kWidth * .5f * m_scale / GetScreenHeight());
        m_camera.target = Vector3Add(center, offset);
        m_camera.position = Vector3Add(m_camera.target, Vector3Scale(view, 4.0f));
        m_camera.up = {0, 1, 0};
        m_camera.fovy = m_scale;
        m_camera.projection = CAMERA_ORTHOGRAPHIC;

        if (m_weights)
        {
            const int delta = clicked(kNextJoint) - clicked(kPreviousJoint) +
                              keyStep(KEY_RIGHT) - keyStep(KEY_LEFT);
            if (delta)
            {
                m_joint = (m_joint + delta + scene.joints()) % scene.joints();
                scene.heatWeights(m_joint);
            }
            if (clicked(bindPose())) m_bind = !m_bind;
            if (m_mesh && mouse.x > kWidth && !hovered(skeletonButton()) && !hovered(meshButton()) &&
                !hovered(zoomOut()) && !hovered(zoomIn()) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                m_selected = scene.pickVertex(mouse, m_camera, origin, facing);
            if (m_selected)
            {
                const int change = clicked(kMore) - clicked(kLess);
                if (change)
                {
                    m_status = scene.adjustWeight(*m_selected, m_joint, .05f * change)
                               ? "Edici\xc3\xb3n local" : "Sin cambio. Prueba otro hueso.";
                }
                if (clicked(kRestore))
                {
                    scene.resetWeights(*m_selected);
                    m_status = "Pesos originales del v\xc3\xa9rtice";
                }
            }
            if (clicked(kSave))
                m_status = scene.saveWeights() ? "Ajustes guardados" : "Error al guardar";
        }
        else
        {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                m_searching = hovered(kSearch);
            if (m_searching)
            {
                for (int key = GetCharPressed(); key; key = GetCharPressed())
                    if (key >= 32 && key < 127 && m_search.size() < 22)
                    {
                        m_search.push_back(static_cast<char>(key));
                        m_offset = 0;
                    }
                if (IsKeyPressed(KEY_BACKSPACE) && !m_search.empty())
                {
                    m_search.pop_back();
                    m_offset = 0;
                }
                if (IsKeyPressed(KEY_ENTER)) m_searching = false;
            }
            const auto query = lower(m_search);
            m_filtered.clear();
            for (int i = 0; i < scene.clipCount(); ++i)
                if (lower(scene.clipLabel(i)).find(query) != std::string::npos)
                    m_filtered.push_back(i);
            browseAnimations();

            const int last = scene.clipFrames(m_clip) - 1;
            if (clicked(playback()) || (!m_searching && IsKeyPressed(KEY_SPACE)))
            {
                m_play = !m_play;
                m_bind = false;
                if (m_play && m_frame == last) m_time = 0;
            }
            if (clicked(loop())) m_loop = !m_loop;
            if (clicked(bindPose())) m_bind = !m_bind;
            const int frameStep = m_searching ? 0 : keyStep(KEY_RIGHT) - keyStep(KEY_LEFT);
            if (frameStep)
            {
                m_play = false;
                m_bind = false;
                m_frame = std::clamp(m_frame + frameStep, 0, last);
                m_time = m_frame * kFrameStep;
            }
            const auto bar = timeline();
            if (hovered(bar) && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            {
                m_play = false;
                m_bind = false;
                m_frame = static_cast<int>(std::lround(std::clamp((mouse.x - bar.x) / bar.width, 0.0f, 1.0f) * last));
                m_time = m_frame * kFrameStep;
            }
            if (m_play && !m_bind)
            {
                m_time += dt;
                if (m_time >= (last + 1) * kFrameStep)
                {
                    if (m_loop) m_time = std::fmod(m_time, (last + 1) * kFrameStep);
                    else { m_time = last * kFrameStep; m_play = false; }
                }
                m_frame = std::clamp(static_cast<int>(std::lround(m_time / kFrameStep)), 0, last);
            }
        }
        scene.inspect(m_clip, m_frame, m_bind);
    }

    void RigInspector::draw(const Scene &scene, Vector3 origin, float facing) const
    {
        ClearBackground(kBackdrop);
        BeginMode3D(m_camera);
        for (int i = -10; i <= 10; ++i)
        {
            const float lane = i * .25f;
            const float ground = origin.y - .006f;
            constexpr Color line{42, 45, 49, 255};
            DrawLine3D({origin.x + lane, ground, origin.z - 2.5f}, {origin.x + lane, ground, origin.z + 2.5f}, line);
            DrawLine3D({origin.x - 2.5f, ground, origin.z + lane}, {origin.x + 2.5f, ground, origin.z + lane}, line);
        }
        if (m_mesh)
            scene.draw(origin, facing, m_weights, false);
        EndMode3D();
        if (m_skeleton)
            drawSkeleton(scene, origin, facing);
        if (m_mesh && m_weights && m_selected)
        {
            const auto point = GetWorldToScreen(scene.vertexPosition(*m_selected, origin, facing), m_camera);
            DrawCircleV(point, 6, kPanel);
            DrawRing(point, 4, 6, 0, 360, 20, kCopper);
        }
        panel(scene);
        control(skeletonButton(), "Esqueleto", m_skeleton);
        control(meshButton(), "Malla", m_mesh);
        control(zoomOut(), "-");
        control(zoomIn(), "+");
        DrawRectangle(kWidth + 1, GetScreenHeight() - 47, GetScreenWidth() - kWidth, 47, Color{30, 31, 34, 255});
        label("A / D o clic derecho  Girar    |    F7  Cerrar", kWidth + 20, GetScreenHeight() - 31, 16, kInk);
    }

    void RigInspector::drawSkeleton(const Scene &scene, Vector3 origin, float facing) const
    {
        constexpr Color shadow{25, 29, 35, 255};
        for (int joint = 1; joint < scene.joints(); ++joint)
        {
            const int parent = scene.jointParent(joint);
            if (parent <= 0)
                continue;
            const auto a = GetWorldToScreen(scene.jointPosition(parent, origin, facing), m_camera);
            const auto b = GetWorldToScreen(scene.jointPosition(joint, origin, facing), m_camera);
            if (Vector2DistanceSqr(a, b) < .01f)
                continue;
            const Color wire = joint >= 19 && joint <= 22 ? kCopper :
                               joint >= 23 && joint <= 26 ? Color{118, 211, 225, 255} : kInk;
            DrawLineEx(a, b, 4.5f, shadow);
            DrawLineEx(a, b, 2.0f, wire);
            DrawCircleV(b, 4.5f, shadow);
            DrawCircleV(b, 2.6f, wire);
        }
    }

    void RigInspector::panel(const Scene &scene) const
    {
        DrawRectangle(0, 0, kWidth, GetScreenHeight(), kPanel);
        DrawRectangle(kWidth - 1, 0, 1, GetScreenHeight(), kControl);
        label("Inspector de Gene", 20, 23, 26, kInk);
        control(kAnimations, "Animaciones", !m_weights);
        control(kWeights, "Pesos", m_weights);
        if (m_weights)
        {
            label("Hueso", 20, 111, 16, kMuted);
            control(kPreviousJoint, "<");
            control(kNextJoint, ">");
            const char *joint = scene.jointName(m_joint);
            label(joint, static_cast<int>((kWidth - textWidth(joint, 20)) / 2), 143, 20, kInk);
            DrawRectangleGradientH(20, 184, 302, 10, Color{49, 67, 90, 255}, Color{255, 143, 66, 255});
            label("0 %", 20, 202, 15, kMuted);
            label("100 %", 275, 202, 15, kMuted);
            label(m_selected ? TextFormat("Malla %d  /  V\xc3\xa9rtice %d", m_selected->mesh, m_selected->index)
                                : "Selecciona un v\xc3\xa9rtice en Gene", 20, 236, 17, kInk);
            if (m_selected)
            {
                int i = 0;
                for (const auto influence : scene.weights(*m_selected))
                {
                    const int y = 269 + 27 * i++;
                    label(influence.weight > 0 ? scene.jointName(influence.joint) : "-", 20, y, 15, kMuted);
                    DrawRectangle(164, y + 2, 103, 11, kControl);
                    DrawRectangle(164, y + 2, static_cast<int>(103 * influence.weight), 11,
                                  influence.joint == m_joint ? kCopper : kMuted);
                    label(TextFormat("%3.0f %%", influence.weight * 100), 276, y, 15, kInk);
                }
            }
            control(kLess, "-5 %", false, m_selected.has_value());
            control(kMore, "+5 %", false, m_selected.has_value());
            control(kRestore, "Restaurar", false, m_selected.has_value());
            control(kSave, "Guardar");
            label(m_status, 20, 486, 15, kCopper);
            control(bindPose(), "Pose base", m_bind);
            return;
        }
        DrawRectangleRec(kSearch, kControl);
        if (m_searching) DrawRectangleLinesEx(kSearch, 1, kCopper);
        label(m_search.empty() ? "Buscar por MOT o nombre" : m_search.c_str(), 30, 123, 16,
                 m_search.empty() ? kMuted : kInk);
        const int count = static_cast<int>(m_filtered.size());
        for (int i = 0; i < rows() && i + m_offset < count; ++i)
        {
            const int index = m_filtered[i + m_offset];
            const auto rect = row(i);
            const bool selected = index == m_clip;
            if (selected || hovered(rect)) DrawRectangleRec(rect, selected ? kControl : Color{36, 33, 30, 255});
            if (selected) DrawRectangle(20, static_cast<int>(rect.y), 3, 27, kCopper);
            label(scene.clipLabel(index), 30, static_cast<int>(rect.y) + 5, 16, selected ? kInk : kMuted);
            label(TextFormat("%d f", scene.clipFrames(index)), 254, static_cast<int>(rect.y) + 6, 14, kMuted);
        }
        if (count > rows())
        {
            const auto track = scrollbar();
            const auto thumb = scrollThumb(track, count, m_offset);
            DrawRectangleRec(track, kControl);
            DrawRectangleRec(thumb, m_listDragging || hovered(track) ? kCopper : kMuted);
        }
        label(TextFormat("%d de %d MOT   |   Arriba / Abajo", count, scene.clipCount()),
              20, static_cast<int>(timelineY()) - 23, 15, kMuted);
        control(playback(), m_play && !m_bind ? "Pausar" : "Reproducir");
        control(loop(), "Bucle", m_loop);
        control(bindPose(), "Pose base", m_bind);
        const auto bar = timeline();
        DrawRectangleRec(bar, kControl);
        const int frames = scene.clipFrames(m_clip);
        DrawRectangleRec({bar.x, bar.y, bar.width * m_frame / (frames - 1), bar.height}, kCopper);
        const char *position = m_bind ? "Pose base" : TextFormat("Fotograma %d / %d", m_frame + 1, frames);
        label(position, 20, static_cast<int>(bar.y) + 23, 15, kInk);
        label("Izq. / Der.", 252, static_cast<int>(bar.y) + 23, 15, kMuted);
    }
}
