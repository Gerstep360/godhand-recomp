#include "pc_media.h"
#include "raymath.h"
#include "rig_inspector.h"

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
        bool front = false;
        bool exercise = false;
        bool combatTest = false;
        bool inspectorTest = false;
        std::string_view inspector;
        std::optional<float> yaw;
    };

    Options parseOptions(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            if (arg == "--hidden")
                options.hidden = true;
            else if (arg == "--front")
                options.front = true;
            else if (arg == "--exercise")
                options.exercise = true;
            else if (arg == "--exercise-combat")
                options.combatTest = true;
            else if (arg == "--exercise-inspector")
                options.inspectorTest = true;
            else if (arg == "--inspect" && i + 1 < argc)
                options.inspector = argv[++i];
            else if (arg == "--yaw" && i + 1 < argc)
                options.yaw = std::stof(argv[++i]);
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
                throw std::runtime_error("Uso: godhand-native [--assets carpeta] [--frames N] [--hidden] [--front] [--yaw rad] [--exercise] [--exercise-combat] [--exercise-inspector] [--inspect anim|weights|bones] [--capture imagen.png]");
        }
        if ((options.hidden || options.capture || options.exercise || options.combatTest || options.inspectorTest) && !options.frames)
            throw std::runtime_error("--hidden, --capture y las pruebas requieren --frames.");
        if (!options.inspector.empty() && options.inspector != "anim" && options.inspector != "weights" && options.inspector != "bones")
            throw std::runtime_error("--inspect admite anim, weights o bones.");
        if (options.yaw && !std::isfinite(*options.yaw))
            throw std::runtime_error("--yaw requiere un angulo finito.");
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
            SetWindowMinSize(900, 620);
            SetExitKey(KEY_NULL);
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
        SetTraceLogLevel(LOG_WARNING);
        Devices devices(options.hidden);
        const double loadStart = GetTime();
        auto scene = godhand::pc::Scene::load(options.assets);
        const double loadMs = (GetTime() - loadStart) * 1000.0;
        bool muted = options.hidden;
        scene.setMuted(muted);
        scene.play();
        std::printf("PC scene: %zu room models, %u triangles, %d joints, %d clips, %d idle poses, %u audio channels, %.0f ms load\n",
                    scene.stageParts(), scene.triangles(), scene.joints(), scene.clipCount(), scene.poseCount(), scene.audioChannels(), loadMs);

        Camera3D camera{};
        camera.up = {0.0f, 1.0f, 0.0f};
        camera.fovy = 56.0f;
        camera.projection = CAMERA_PERSPECTIVE;
        Vector3 spawn{0.0f, 0.0f, 7.5f};
        const auto &world = scene.collision();
        spawn.y = world.ground(spawn, 0.35f, 0.8f).value_or(spawn.y);
        godhand::pc::CharacterController actor(spawn);
        constexpr float initialYaw = 0.45f;
        float yaw = options.yaw.value_or(initialYaw + (options.front ? PI : 0.0f));
        float smoothYaw = yaw;
        float pitch = 0.10f;
        float smoothPitch = pitch;
        float facing = (initialYaw + PI) * RAD2DEG;
        float cameraHeight = spawn.y;
        float cameraDistance = 3.2f;
        const int firstPose = scene.pose();
        bool poseChanged = false;
        bool walked = false, ran = false;
        bool struck = false, registeredHit = false;
        float jumpHeight = 0, hitFlash = 0;
        const Vector3 startForward{-std::sin(initialYaw), 0, -std::cos(initialYaw)};
        const Vector3 practiceTarget = Vector3Add(spawn, Vector3Add(Vector3Scale(startForward, .79f), {0, 1.54f, 0}));
        constexpr int kRightFist = 11;
        auto oldFist = scene.jointPosition(kRightFist, spawn, facing);
        bool hitThisStrike = false;
        unsigned frames = 0;

        bool inspecting = !options.inspector.empty();
        unsigned inspectorOpens = 0, inspectorCloses = 0;
        godhand::pc::RigInspector inspector(scene);
        if (options.inspector == "weights")
            inspector.showWeights(scene);
        if (options.inspector == "bones")
            inspector.showSkeleton();
        bool mouseLocked = !options.hidden && !inspecting;
        bool lockedBeforeInspector = !options.hidden;
        if (mouseLocked)
            DisableCursor();

        while (!WindowShouldClose() && (options.frames == 0 || frames < options.frames))
        {
            const float dt = std::clamp(GetFrameTime(), 0.0f, 0.1f);

            const bool escape = IsKeyPressed(KEY_ESCAPE);
            if (IsKeyPressed(KEY_M))
            {
                muted = !muted;
                scene.setMuted(muted);
            }
            const bool testToggle = options.inspectorTest && (frames == 12 || frames == 84 || frames == 106 || frames == 138);
            const bool toggleInspector = IsKeyPressed(KEY_F7) || (escape && inspecting) || testToggle;
            if (toggleInspector)
            {
                inspecting = !inspecting;
                if (inspecting)
                {
                    ++inspectorOpens;
                    lockedBeforeInspector = mouseLocked;
                    mouseLocked = false;
                    EnableCursor();
                }
                else
                {
                    ++inspectorCloses;
                    scene.leaveInspection();
                    mouseLocked = lockedBeforeInspector;
                    if (mouseLocked) DisableCursor();
                }
                // Finish drawing this frame so raylib also polls and clears
                // the key transition before the next iteration.
            }
            if (inspecting)
            {
                if (options.inspectorTest && frames == 48)
                    inspector.showWeights(scene);
                inspector.update(scene, actor.position, facing, dt);
                scene.update(dt);
                poseChanged |= scene.pose() != firstPose;
                BeginDrawing();
                inspector.draw(scene, actor.position, facing);
                EndDrawing();
                ++frames;
                continue;
            }
            if (escape && !toggleInspector && !mouseLocked)
                break;
            const bool recapture = !mouseLocked && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
            const bool toggleCursor = !toggleInspector && (IsKeyPressed(KEY_TAB) || escape || recapture);
            if (toggleCursor)
            {
                mouseLocked = !mouseLocked;
                if (mouseLocked)
                    DisableCursor();
                else
                    EnableCursor();
            }
            if (mouseLocked && !toggleCursor && !toggleInspector)
            {
                const Vector2 mouseDelta = GetMouseDelta();
                constexpr float mouseSens = 0.003f;
                yaw += mouseDelta.x * mouseSens;
                pitch += mouseDelta.y * mouseSens;
            }

            // Keyboard camera rotation
            yaw += static_cast<float>(IsKeyDown(KEY_RIGHT) - IsKeyDown(KEY_LEFT)) * 1.65f * dt;
            pitch += static_cast<float>(IsKeyDown(KEY_DOWN) - IsKeyDown(KEY_UP)) * 1.35f * dt;
            pitch = std::clamp(pitch, -0.31f, 0.68f);
            const float aimBlend = 1.0f - std::exp(-24.0f * dt);
            smoothYaw += std::remainder(yaw - smoothYaw, 2 * PI) * aimBlend;
            smoothPitch += (pitch - smoothPitch) * aimBlend;

            if (IsKeyPressed(KEY_SPACE) || (options.combatTest && frames == 60))
                actor.jump();
            if ((mouseLocked && !toggleCursor && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ||
                (options.combatTest && frames == 16))
                scene.attack();

            const Vector3 forward{-std::sin(smoothYaw), 0.0f, -std::cos(smoothYaw)};
            const Vector3 right{std::cos(smoothYaw), 0.0f, -std::sin(smoothYaw)};
            const bool testMoving = options.exercise && frames >= 30 && frames < options.frames - options.frames / 6;
            const float longitudinal = testMoving ? 1.0f : static_cast<float>(IsKeyDown(KEY_W) - IsKeyDown(KEY_S));
            const float lateral = static_cast<float>(IsKeyDown(KEY_D) - IsKeyDown(KEY_A));
            const bool sprint = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ||
                                 (testMoving && frames >= options.frames / 2)) && longitudinal > 0;
            const auto direction = Vector3Normalize(Vector3Add(Vector3Scale(forward, longitudinal), Vector3Scale(right, lateral)));
            actor.update(world, direction, sprint ? 6.2f : 3.45f, dt);
            jumpHeight = std::max(jumpHeight, actor.position.y - spawn.y);
            if (!options.front && !options.yaw)
                facing += std::remainder((smoothYaw + PI) * RAD2DEG - facing, 360.0f) * aimBlend;

            const auto position = actor.position;
            cameraHeight += (position.y - cameraHeight) * (1.0f - std::exp(-13.0f * dt));
            const Vector3 chest{position.x, cameraHeight + 1.42f, position.z};
            const float shoulder = std::max(0.0f, world.trace(chest, right, 0.68f) - 0.08f);
            const auto pivot = Vector3Add(chest, Vector3Scale(right, shoulder));
            const Vector3 arm = Vector3Add(Vector3Scale(forward, -std::cos(smoothPitch)), {0.0f, std::sin(smoothPitch), 0.0f});
            const float distance = std::clamp(world.trace(pivot, arm, 3.35f) - 0.17f, 0.23f, 3.18f);
            cameraDistance = distance < cameraDistance ? distance :
                             cameraDistance + (distance - cameraDistance) * (1.0f - std::exp(-8.0f * dt));
            camera.target = Vector3Add(pivot, Vector3Scale(forward, 0.6f));
            camera.position = Vector3Add(pivot, Vector3Scale(arm, cameraDistance));
            scene.update(dt, actor.grounded() ? actor.planarSpeed() : 0.0f, !actor.grounded());
            const auto fist = scene.jointPosition(kRightFist, position, facing);
            hitFlash = std::max(0.0f, hitFlash - dt);
            if (!scene.striking())
                hitThisStrike = false;
            if (scene.striking() && !hitThisStrike && scene.pose() >= 5 && scene.pose() <= 13)
            {
                const auto sweep = Vector3Subtract(fist, oldFist);
                const float lengthSquared = Vector3LengthSqr(sweep);
                const float along = lengthSquared > 1e-8f ?
                    std::clamp(Vector3DotProduct(Vector3Subtract(practiceTarget, oldFist), sweep) / lengthSquared, 0.0f, 1.0f) : 0;
                const auto contact = Vector3Add(oldFist, Vector3Scale(sweep, along));
                if (Vector3DistanceSqr(contact, practiceTarget) < .23f * .23f)
                {
                    registeredHit = hitThisStrike = true;
                    hitFlash = .18f;
                }
            }
            oldFist = fist;
            struck |= scene.striking();
            poseChanged |= scene.pose() != firstPose;
            walked |= std::string_view(scene.clipName()) == "walk";
            ran |= std::string_view(scene.clipName()) == "run";

            BeginDrawing();
            ClearBackground(Color{124, 160, 190, 255});
            BeginMode3D(camera);
            scene.draw(position, facing);
            // The target gives the native scene a repeatable contact for the punch.
            const Vector3 foot{practiceTarget.x, spawn.y, practiceTarget.z};
            DrawCylinder(foot, .07f, .18f, 1.42f, 12, Color{78, 59, 47, 255});
            DrawSphere(practiceTarget, .17f, hitFlash > 0 ? Color{249, 154, 70, 255} : Color{132, 84, 59, 255});
            EndMode3D();

            const int line = GetScreenHeight() - 44;
            const char *controls = mouseLocked || options.hidden
                ? "WASD Mover  |  Shift Correr  |  Espacio Saltar  |  Clic izq. Golpear  |  F7 Inspector  |  Tab Cursor"
                : "Clic o Tab Capturar raton  |  Esc Salir";
            DrawRectangle(16, line, MeasureText(controls, 14) + 18, 29, Color{20, 25, 28, 220});
            DrawText(controls, 25, line + 7, 14, RAYWHITE);
            EndDrawing();
            ++frames;
        }

        if (mouseLocked)
            EnableCursor();

        if (options.capture)
            TakeScreenshot(options.capture->string().c_str());
        if (options.exercise)
            std::printf("Movement: walk=%d run=%d distance=%.2f m\n", walked, ran, Vector3Distance(spawn, actor.position));
        if (options.combatTest)
            std::printf("Combat: strike=%d hit=%d jump=%.2f m landed=%d\n", struck, registeredHit, jumpHeight, actor.grounded());
        if (options.inspectorTest)
            std::printf("Inspector: entered=%u exited=%u\n", inspectorOpens, inspectorCloses);
        std::printf("PC scene: %u frames, animation advanced=%s, music played=%.3f s\n",
                    frames, poseChanged ? "yes" : "no", scene.musicTime());
        if ((options.frames && (frames != options.frames || (!poseChanged && options.inspector != "weights") || scene.musicTime() <= 0.0f)) ||
            (options.exercise && (!walked || !ran || Vector3Distance(spawn, actor.position) < 1.0f)) ||
            (options.combatTest && (!struck || !registeredHit || jumpHeight < .35f || !actor.grounded())) ||
            (options.inspectorTest && (inspectorOpens != 2 || inspectorCloses != 2)))
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
