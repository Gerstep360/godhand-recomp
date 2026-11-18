#include "pc_media.h"
#include "raymath.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
    struct Options
    {
        std::filesystem::path assets = std::filesystem::path(GetApplicationDirectory()) / "pc_assets";
        std::optional<std::filesystem::path> capture;
        unsigned frames = 0;
        bool hidden = false;
    };

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            if (arg == "--hidden")
                options.hidden = true;
            else if (arg == "--assets" && i + 1 < argc)
                options.assets = argv[++i];
            else if (arg == "--capture" && i + 1 < argc)
                options.capture = argv[++i];
            else if (arg == "--frames" && i + 1 < argc)
            {
                const std::string_view value(argv[++i]);
                const auto result = std::from_chars(value.data(), value.data() + value.size(), options.frames);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.frames == 0)
                    throw std::runtime_error("--frames requiere un entero positivo.");
            }
            else
                throw std::runtime_error("Uso: godhand-native [--assets carpeta] [--frames N] [--hidden] [--capture imagen.png]");
        }
        if ((options.hidden || options.capture) && !options.frames)
            throw std::runtime_error("--hidden y --capture requieren --frames.");
        return options;
    }

    class Devices
    {
    public:
        explicit Devices(bool hidden)
        {
            SetConfigFlags(FLAG_WINDOW_RESIZABLE | (hidden ? FLAG_WINDOW_HIDDEN : FLAG_VSYNC_HINT));
            InitWindow(1280, 720, "God Hand PC - Escena nativa");
            if (!IsWindowReady())
                throw std::runtime_error("No se pudo abrir la ventana de PC.");
            InitAudioDevice();
            if (!IsAudioDeviceReady())
            {
                CloseWindow();
                throw std::runtime_error("No se pudo abrir el dispositivo de audio.");
            }
            SetTargetFPS(120);
        }

        ~Devices()
        {
            CloseAudioDevice();
            CloseWindow();
        }

        Devices(const Devices &) = delete;
        Devices &operator=(const Devices &) = delete;
    };

    int run(const Options &options)
    {
        if (!options.hidden)
            SetTraceLogLevel(LOG_WARNING);
        Devices devices(options.hidden);
        const double loadStart = GetTime();
        auto scene = godhand::pc::Scene::load(options.assets);
        const double loadMs = (GetTime() - loadStart) * 1000.0;
        bool muted = options.hidden;
        bool paused = false;
        scene.setMuted(muted);
        scene.play();
        std::printf("PC scene: %zu room models, %u triangles, %d joints, %d poses, %u audio channels, %.0f ms load\n",
                    scene.stageParts(), scene.triangles(), scene.joints(), scene.poseCount(), scene.audioChannels(), loadMs);

        Camera3D camera{};
        camera.up = {0.0f, 1.0f, 0.0f};
        camera.fovy = 56.0f;
        camera.projection = CAMERA_PERSPECTIVE;
        Vector3 position{0.0f, 0.0f, 7.5f};
        position.y = scene.floorHeight(position);
        float yaw = 0.45f;
        float pitch = 0.22f;
        float facing = 200.0f;
        const int firstPose = scene.pose();
        bool poseChanged = false;
        unsigned frames = 0;

        bool mouseLocked = !options.hidden;
        if (mouseLocked)
            DisableCursor();

        while (!WindowShouldClose() && (options.frames == 0 || frames < options.frames))
        {
            const float dt = std::clamp(GetFrameTime(), 0.0f, 0.1f);

            // Toggle mouse capture with Tab or Escape; click into window to lock
            if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_ESCAPE))
            {
                mouseLocked = !mouseLocked;
                if (mouseLocked)
                    DisableCursor();
                else
                    EnableCursor();
            }
            if (!mouseLocked && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                mouseLocked = true;
                DisableCursor();
            }

            // Camera control via mouse when captured
            if (mouseLocked)
            {
                const Vector2 mouseDelta = GetMouseDelta();
                constexpr float mouseSens = 0.003f;
                yaw += mouseDelta.x * mouseSens;
                pitch = std::clamp(pitch + mouseDelta.y * mouseSens, -0.35f, 1.10f);
            }

            // Keyboard camera rotation
            yaw += static_cast<float>(IsKeyDown(KEY_RIGHT) - IsKeyDown(KEY_LEFT)) * 1.65f * dt;
            pitch += static_cast<float>(IsKeyDown(KEY_DOWN) - IsKeyDown(KEY_UP)) * 1.35f * dt;
            pitch = std::clamp(pitch, -0.35f, 1.10f);

            if (IsKeyPressed(KEY_M))
            {
                muted = !muted;
                scene.setMuted(muted);
            }
            if (IsKeyPressed(KEY_SPACE))
            {
                paused = !paused;
                scene.pauseAnimation(paused);
            }

            const Vector3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
            const Vector3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
            const float longitudinal = static_cast<float>(IsKeyDown(KEY_W) - IsKeyDown(KEY_S));
            const float lateral = static_cast<float>(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
            if (longitudinal != 0.0f || lateral != 0.0f)
            {
                const auto direction = Vector3Normalize(Vector3Add(Vector3Scale(forward, longitudinal), Vector3Scale(right, lateral)));
                position = Vector3Add(position, Vector3Scale(direction, 3.5f * dt));
                position.y = scene.floorHeight(position);
                facing = std::atan2(direction.x, direction.z) * RAD2DEG;
            }

            const float camDist = 4.25f;
            const float horizDist = std::cos(pitch) * camDist;
            const float vertOffset = 1.30f + std::sin(pitch) * camDist;
            const Vector3 shoulder = Vector3Scale(right, 0.45f);
            camera.target = Vector3Add(position, Vector3Add(shoulder, Vector3{0.0f, 1.30f, 0.0f}));
            camera.position = Vector3Add(position, Vector3Add(shoulder, Vector3Add(Vector3Scale(forward, -horizDist), Vector3{0.0f, vertOffset, 0.0f})));
            scene.update(dt);
            poseChanged |= scene.pose() != firstPose;

            BeginDrawing();
            ClearBackground(Color{124, 160, 190, 255});
            BeginMode3D(camera);
            scene.draw(position, facing);
            EndMode3D();

            const int line = GetScreenHeight() - 44;
            if (mouseLocked)
            {
                DrawRectangle(16, line, 680, 29, Color{20, 25, 28, 220});
                DrawText("WASD Mover | Mouse / Flechas Camara | TAB / ESC Liberar cursor | Espacio Pausa | M Audio", 25, line + 7, 14, RAYWHITE);
            }
            else
            {
                DrawRectangle(16, line, 680, 29, Color{180, 45, 45, 230});
                DrawText("[MOUSE LIBRE] Clic Izquierdo o TAB para volver a bloquear el cursor y mover la camara", 25, line + 7, 14, WHITE);
            }
            EndDrawing();
            ++frames;
        }

        if (mouseLocked)
            EnableCursor();

        if (options.capture)
            TakeScreenshot(options.capture->string().c_str());
        std::printf("PC scene: %u frames, animation advanced=%s, music played=%.3f s\n",
                    frames, poseChanged ? "yes" : "no", scene.musicTime());
        if (options.frames && (frames != options.frames || !poseChanged || scene.musicTime() <= 0.0f))
            return 2;
        return 0;
    }
}

int main(int argc, char **argv)
{
    try
    {
        return run(parseOptions(argc, argv));
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
