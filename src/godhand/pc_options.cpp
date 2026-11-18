#include "godhand/pc_options.h"
#ifdef GODHAND_WITH_EXTRAS
#include "gh_extras/host.h"
#endif
#include "godhand/function_hooks.h"
#include "ps2_runtime_macros.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_host_ui.h"
#include "runtime/ps2_controls_screen.h"
#include "runtime/ps2_cd_image.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include <rlgl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace godhand::pcoptions
{
    namespace
    {
        std::filesystem::path settings;
        Font font{};
        bool owned = false;
        int resolution = 1, prepare = 0;
        std::string status;
        std::vector<Color> glyphPixels;
        uint32_t u32(const std::vector<uint8_t> &b, size_t o)
        { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4); return v; }

        int gameGlyph(unsigned c)
        {
            if (c >= 'A' && c <= 'Z') return 64 + c - 'A';
            if (c >= 'a' && c <= 'z') return 90 + c - 'a';
            if (c >= '0' && c <= '9') return 54 + c - '0';
            // Explicit mapping follows the paired glyphs in the original US core font.
            switch (c)
            {
            case '=': return 32; case '.': return 33; case '?': return 34; case '!': return 35;
            case '$': return 36; case '%': return 37; case '&': return 38; case '\'': return 39;
            case '(': return 40; case ')': return 41; case '{': return 42; case '}': return 43;
            case '_': return 44; case '/': return 45; case '^': return 46; case '|': return 47;
            case '@': return 48; case ':': return 49; case '[': return 50; case ']': return 51;
            case ';': return 52; case '+': return 14; case '-': return 15; case ',': return 17;
            default: return 34;
            }
        }

        bool loadGameFont()
        {
            auto &disc = PS2CdImage::instance();
            PS2CdImage::Entry afs{};
            if (!disc.find("_DATA.AFS", afs)) return false;
            uint32_t entry[2]{};
            const uint64_t base = uint64_t(afs.lba) * PS2CdImage::kSectorSize;
            if (!disc.read(base + 8 + 8 * 8, entry, sizeof(entry)) || entry[1] < 0x56e90 || entry[1] > 8 * 1024 * 1024)
                return false;
            std::vector<uint8_t> core(entry[1]);
            if (!disc.read(base + entry[0], core.data(), core.size())) return false;
            // core_usa.bin, original US font texture: CT32 upload -> 512x256 PSMT4.
            // Each 32x32 cell packs two glyphs in independent two-bit alpha planes.
            constexpr size_t record = 0x468b0, payload = record + 0xe0;
            if (u32(core, record) != 1 || u32(core, record + 4) != 0x01000040 || payload + 65536 > core.size())
                return false;
            std::vector<uint8_t> vram(65536);
            for (uint32_t y = 0; y < 256; ++y)
                for (uint32_t x = 0; x < 64; ++x)
                {
                    const auto a = GSPSMCT32::addrPSMCT32(0, 1, x, y);
                    std::memcpy(vram.data() + a, core.data() + payload + (y * 64 + x) * 4, 4);
                }
            Image atlas = GenImageColor(512, 192, BLANK);
            auto *pixels = static_cast<Color *>(atlas.data);
            font = {};
            font.baseSize = 32;
            font.glyphCount = 95;
            font.recs = static_cast<Rectangle *>(MemAlloc(sizeof(Rectangle) * 95));
            font.glyphs = static_cast<GlyphInfo *>(MemAlloc(sizeof(GlyphInfo) * 95));
            if (!font.recs || !font.glyphs) { UnloadImage(atlas); MemFree(font.recs); MemFree(font.glyphs); font = {}; return false; }
            std::memset(font.glyphs, 0, sizeof(GlyphInfo) * 95);
            constexpr uint8_t alpha[] = {0, 255, 192, 128};
            for (int i = 0; i < 95; ++i)
            {
                const unsigned c = unsigned(i + 32);
                const int g = gameGlyph(c), cell = g / 2, shift = (g % 2 == 0) ? 2 : 0;
                const int sx = (cell % 16) * 32, sy = (cell / 16) * 32;
                const int dx = (i % 16) * 32, dy = (i / 16) * 32;
                int left = 32, right = 0;
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x)
                    {
                        const auto a = GSPSMT4::addrPSMT4(0, 8, sx + x, sy + y);
                        const auto nibble = (vram[a / 2] >> ((a & 1) * 4)) & 15;
                        const uint8_t opacity = c == ' ' ? 0 : alpha[(nibble >> shift) & 3];
                        pixels[(dy + y) * 512 + dx + x] = Color{255, 255, 255, opacity};
                        if (opacity) { left = std::min(left, x); right = std::max(right, x); }
                    }
                if (c == ' ') { left = 0; right = 9; }
                const int width = std::max(1, right - left + 1);
                font.recs[i] = Rectangle{float(dx + left), float(dy), float(width), 32};
                font.glyphs[i].value = int(c);
                font.glyphs[i].advanceX = width + 2;
            }
            glyphPixels.assign(pixels, pixels + 512 * 192);
            font.texture = LoadTextureFromImage(atlas);
            UnloadImage(atlas);
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            owned = font.texture.id != 0;
            std::fprintf(stderr, "[pc-options] original US font loaded from the player's disc\n");
            return owned;
        }

        bool writeSetting(const std::string &section, const std::string &key, const std::string &value)
        {
            std::ifstream in(settings);
            if (!in) { status = "NO SE PUDO LEER LA CONFIGURACION"; return false; }
            std::vector<std::string> lines;
            std::string line, current;
            bool changed = false, foundSection = false;
            size_t insert = 0;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && line.front() == '[')
                {
                    current = line;
                    if (current == "[" + section + "]") { foundSection = true; insert = lines.size() + 1; }
                }
                if (current == "[" + section + "]" && line.rfind(key + "=", 0) == 0)
                { line = key + "=" + value; changed = true; }
                lines.push_back(line);
            }
            if (!changed)
            {
                if (!foundSection) { lines.push_back("[" + section + "]"); insert = lines.size(); }
                lines.insert(lines.begin() + insert, key + "=" + value);
            }
            // Keep the previous file available for recovery if the write is interrupted.
            std::error_code ec;
            std::filesystem::copy_file(settings, settings.string() + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { status = "NO SE PUDO GUARDAR LA CONFIGURACION"; return false; }
            std::ofstream out(settings, std::ios::binary | std::ios::trunc);
            for (const auto &s : lines) out << s << "\r\n";
            out.flush();
            if (!out) { status = "ERROR AL GUARDAR - COPIA EN GODHAND.INI.BAK"; return false; }
            return true;
        }

        // Decode the player's IDD resources, retaining the original indexed palette and upload layout.
        // The resulting payload is consumed by the game's own ID renderer, not drawn over its framebuffer.
        struct NativeTexture
        {
            std::vector<uint8_t> file, payload;
            std::vector<uint32_t> offsets;
            std::vector<Color> pixels;
            std::array<Color, 256> palette{};
            std::array<uint8_t, 32768> nearest{};
            uint32_t record = 0, width = 0, height = 0, address = 0;
            uint64_t applied = 0;
            bool load(unsigned entryIndex, unsigned textureIndex)
            {
                auto &disc = PS2CdImage::instance(); PS2CdImage::Entry afs{};
                if (!disc.find("_DATA.AFS", afs)) return false;
                const uint64_t base = uint64_t(afs.lba) * PS2CdImage::kSectorSize;
                uint32_t entry[2]{};
                if (!disc.read(base + 8 + entryIndex * 8, entry, 8) || entry[1] > 2 * 1024 * 1024) return false;
                file.resize(entry[1]);
                if (!disc.read(base + entry[0], file.data(), file.size())) return false;
                const auto table = u32(file, 8);
                record = table + u32(file, table + 4 + textureIndex * 4);
                if (record + 0xe0 >= file.size()) return false;
                uint64_t tex0 = 0; std::memcpy(&tex0, file.data() + record + 32, 8);
                if (((tex0 >> 20) & 63) != 19) return false; // PSMT8
                width = 1u << ((tex0 >> 26) & 15); height = 1u << ((tex0 >> 30) & 15);
                const auto upload = u32(file, record + 4), uw = upload & 65535, uh = upload >> 16;
                const auto bytes = size_t(uw) * uh * 4;
                if (!uw || !uh || width > 512 || height > 512 || record + 0xe0 + bytes + 0x500 > file.size()) return false;
                payload.assign(file.begin() + record + 0xe0, file.begin() + record + 0xe0 + bytes);
                std::vector<uint32_t> inverse(bytes, ~0u);
                for (uint32_t y = 0; y < uh; ++y) for (uint32_t x = 0; x < uw; ++x)
                {
                    const auto a = GSPSMCT32::addrPSMCT32(0, uw / 64, x, y);
                    for (unsigned b = 0; b < 4; ++b) { if (a + b >= bytes) return false; inverse[a + b] = (y * uw + x) * 4 + b; }
                }
                const auto clut = record + 0xe0 + bytes + 0x100;
                for (unsigned i = 0; i < 256; ++i)
                {
                    unsigned p = (i & ~24u) | ((i & 8) << 1) | ((i & 16) >> 1);
                    auto &c = palette[i]; std::memcpy(&c, file.data() + clut + p * 4, 4); c.a = uint8_t(std::min(255, c.a * 2));
                }
                offsets.resize(width * height); pixels.resize(width * height);
                for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
                {
                    const auto a = GSPSMT8::addrPSMT8(0, (tex0 >> 14) & 63, x, y);
                    if (a >= bytes || inverse[a] == ~0u) return false;
                    offsets[y * width + x] = inverse[a]; pixels[y * width + x] = palette[payload[inverse[a]]];
                }
                for (unsigned rgb = 0; rgb < nearest.size(); ++rgb)
                {
                    const int r = ((rgb >> 10) & 31) * 255 / 31, g = ((rgb >> 5) & 31) * 255 / 31, b = (rgb & 31) * 255 / 31;
                    int best = 1000000; unsigned at = 0;
                    for (unsigned i = 0; i < 256; ++i)
                    {
                        const auto c = palette[i]; if (c.a < 240) continue;
                        const int d = (r-c.r)*(r-c.r) + (g-c.g)*(g-c.g) + (b-c.b)*(b-c.b);
                        if (d < best) { best = d; at = i; }
                    }
                    nearest[rgb] = uint8_t(at);
                }
                return true;
            }
            std::vector<uint8_t> encode(const std::vector<Color> &image) const
            {
                auto out = payload;
                for (size_t i = 0; i < pixels.size(); ++i)
                { const auto c = image[i]; out[offsets[i]] = nearest[((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3)]; }
                return out;
            }
        };
        NativeTexture title, ingame, card;
        Texture2D startupBackground{}, startupWindow{};
        std::vector<Color> controlsBase;
        struct Published { uint64_t version = 0; std::vector<uint8_t> title, ingame; };
        std::mutex publicationMutex;
        std::shared_ptr<const Published> publication;
        std::atomic<bool> nativeActive{false}, leaveNative{false};
        bool hostActive = false, video = false, waitingForOpen = false;
        int videoRow = 0;
        std::string lastSignature;

        void ink(std::vector<Color> &image, const std::string &s, int x, int y, int size, Color color, int maxWidth = 490)
        {
            const int start = x;
            for (unsigned char c : s)
            {
                if (c < 32 || c > 126) c = '?';
                const int i = c - 32; const auto rec = font.recs[i];
                const int advance = std::max(1, int(font.glyphs[i].advanceX * size / 32.0f));
                if (x + advance - start > maxWidth) break;
                for (int dy = 0; dy < size; ++dy) for (int dx = 0; dx < int(rec.width * size / 32); ++dx)
                {
                    int px = x + dx, py = y + dy; if (px < 0 || px >= 512 || py < 0 || py >= 512) continue;
                    auto a = glyphPixels[(int(rec.y) + dy * 32 / size) * 512 + int(rec.x) + dx * 32 / size].a;
                    auto &p = image[py * 512 + px];
                    p = Color{uint8_t((p.r * (255-a) + color.r * a) / 255), uint8_t((p.g * (255-a) + color.g * a) / 255), uint8_t((p.b * (255-a) + color.b * a) / 255), 255};
                }
                x += advance;
            }
        }
        constexpr Color nativeText{218, 167, 113, 255}, highlight{255, 230, 168, 255};

        void initialize()
        {
            if (!loadGameFont()) { font = GetFontDefault(); return; }
            ps2x::controls::setNativePresentation(true);
            if (const char *v = std::getenv("PS2X_GS_SCALE")) resolution = std::clamp(std::atoi(v), 1, 8);
            if (const char *v = std::getenv("PS2X_GS_HW_PRELOAD")) prepare = std::clamp(std::atoi(v), 0, 2);
            if (!title.load(769, 1) || !ingame.load(767, 1) || !card.load(731, 1))
            { std::fprintf(stderr, "[native-options] unsupported IDD resources; original menus retained\n"); title.pixels.clear(); return; }
            controlsBase = title.pixels;
            // Keep the original flaming CONTROLS heading. Reuse the unobstructed bricks below the native diagram.
            for (int y = 76; y < 448; ++y) for (int x = 0; x < 512; ++x)
            {
                auto c = title.pixels[(370 + (y - 76) % 70) * 512 + x];
                const int shade = (x > 28 && x < 484) ? 110 : 180;
                c.r = uint8_t(c.r * shade / 255); c.g = uint8_t(c.g * shade / 255); c.b = uint8_t(c.b * shade / 255);
                controlsBase[y * 512 + x] = c;
            }
            // The boot dialog uses the original memory-card window, brick background and core font.
            Image bg{controlsBase.data(), 512, 512, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            std::vector<Color> boot = controlsBase;
            for (int y = 0; y < 76; ++y) for (int x = 0; x < 512; ++x) boot[y*512+x] = controlsBase[(380+y%60)*512+x];
            bg.data = boot.data(); startupBackground = LoadTextureFromImage(bg);
            Image win{card.pixels.data() + 350 * 512, 512, 82, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            startupWindow = LoadTextureFromImage(win);
            std::fprintf(stderr, "[native-options] original title/game IDD and memory-card resources loaded\n");
        }

        void startup()
        {
            uint32_t done = 0, total = 0; ps2x::hostui::shaderProgress(done, total, true);
            BeginDrawing(); ClearBackground(BLACK);
            const float scale = std::min(GetScreenWidth() / 512.0f, GetScreenHeight() / 448.0f);
            const float ox = (GetScreenWidth() - 512*scale)/2, oy = (GetScreenHeight()-448*scale)/2;
            if (startupBackground.id)
            {
                DrawTexturePro(startupBackground, Rectangle{0,0,512,448}, Rectangle{ox,oy,512*scale,448*scale}, Vector2{}, 0, WHITE);
                DrawTexturePro(startupWindow, Rectangle{0,0,512,82}, Rectangle{ox+10*scale,oy+168*scale,492*scale,112*scale}, Vector2{},0,WHITE);
            }
            auto line = [&](const std::string &s, float y, float size)
            {
                const float w = MeasureTextEx(font,s.c_str(),size*scale,scale).x;
                DrawTextEx(font,s.c_str(),Vector2{ox+(512*scale-w)/2,oy+y*scale},size*scale,scale,highlight);
            };
            line(ps2x::hostui::closing() ? "Cerrando..." : "Preparando datos del juego...", 186, 22);
            line(total ? "Efectos   " + std::to_string(done) + " / " + std::to_string(total) : "Cargando cache grafica...", 222, 19);
            line("Por favor, espera" + std::string(1 + int(GetTime()*2)%3, '.'), 298, 18);
            if (const char *shot = std::getenv("GODHAND_UI_STARTUP_SHOT"))
            {
                static bool taken = false; if (!taken && total && done > 0 && done < total)
                { rlDrawRenderBatchActive(); TakeScreenshot(shot); taken = true; }
            }
            EndDrawing();
        }

        void draw()
        {
#ifdef GODHAND_WITH_EXTRAS
            gh_extras::drawMenu();
#endif
            if (title.pixels.empty()) return;
            if (leaveNative.load()) return;
            const bool active = nativeActive.load();
            if (active && !hostActive)
            {
                hostActive = true; video = false; waitingForOpen = true; lastSignature.clear();
                ps2x::controls::setNativePageInput(true);
                ps2x::controls::requestOpen();
            }
            if (!active && hostActive) { ps2x::controls::saveAndClose(); hostActive = false; }
            if (!hostActive) return;
            if (!ps2x::controls::isOpen())
            {
                // requestOpen is consumed at the start of the next window frame.
                if (waitingForOpen) { waitingForOpen = false; return; }
                leaveNative = true; hostActive = false; return;
            }
            if (const char *shot=std::getenv("GODHAND_UI_OPTIONS_SHOT"))
            {
                static unsigned frames=0; ++frames;
                if (frames==30 || frames==120 || frames==240)
                {
                    const auto p=std::filesystem::path(shot);
                    const auto name=p.stem().string()+"-"+std::to_string(frames)+".png";
                    rlDrawRenderBatchActive(); TakeScreenshot(name.c_str());
                }
            }
            const auto view = ps2x::controls::nativeView();
            std::string testAction;
            // Exercise the actual native page/actions without changing the player's settings or simulating OS input.
            if (std::getenv("GODHAND_UI_SETTINGS_TEST_FILE") && std::getenv("GODHAND_NATIVE_UI_TEST_SCRIPT"))
            {
                static std::stringstream script(std::getenv("GODHAND_NATIVE_UI_TEST_SCRIPT")); static unsigned frames=0;
                if (++frames % 45 == 0 && std::getline(script,testAction,','))
                    std::fprintf(stderr,"[native-options] test action %s\n",testAction.c_str());
            }
            if (!view.capturing && (testAction=="tab" || IsKeyPressed(KEY_TAB) || (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))))
            { video = !video; ps2x::controls::setNativePageInput(!video); status.clear(); }
            if (video)
            {
                const bool pad = IsGamepadAvailable(0);
                auto pressed = [pad](int key,int button) { return IsKeyPressed(key) || (pad && IsGamepadButtonPressed(0,button)); };
                if (testAction=="up" || pressed(KEY_UP,GAMEPAD_BUTTON_LEFT_FACE_UP)) videoRow = (videoRow+3)%4;
                if (testAction=="down" || pressed(KEY_DOWN,GAMEPAD_BUTTON_LEFT_FACE_DOWN)) videoRow = (videoRow+1)%4;
                int direction = testAction=="left" || pressed(KEY_LEFT,GAMEPAD_BUTTON_LEFT_FACE_LEFT) ? -1 : (testAction=="right" || pressed(KEY_RIGHT,GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || pressed(KEY_ENTER,GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) ? 1 : 0;
                if (direction)
                {
                    if (videoRow == 0 && writeSetting("Video","InternalResolution",std::to_string(std::clamp(resolution+direction,1,8)))) resolution = std::clamp(resolution+direction,1,8);
                    if (videoRow == 1) { ToggleBorderlessWindowed(); writeSetting("Video","StartFullscreen",IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)?"1":"0"); }
                    if (videoRow == 2) { int next = prepare == 2 ? 0 : 2; if (writeSetting("Runtime","PS2X_GS_HW_PRELOAD",std::to_string(next))) prepare = next; }
                    if (videoRow == 3 && writeSetting("Video","InternalResolution","1") && writeSetting("Runtime","PS2X_GS_HW_PRELOAD","0") && writeSetting("Runtime","PS2X_GS_DIRECT_PRESENT","1")) { resolution=1; prepare=0; }
                    if (status.empty()) status = "Los cambios se aplican al reiniciar.";
                }
            }
            std::ostringstream signature; signature << video << view.row << view.column << view.capturing << view.status << status << resolution << prepare << videoRow;
            for (const auto &r : view.rows) for (const auto &c : r) signature << c << '\n';
            if (signature.str() == lastSignature) return;
            lastSignature = signature.str(); auto image = controlsBase;
            ink(image, video ? "VIDEO" : "CONFIGURACION DE CONTROLES", 28, 84, 20, highlight);
            if (video)
            {
                std::array<std::string,4> rows{{"Resolucion interna     " + std::to_string(resolution) + "X", "Pantalla completa     " + std::string(IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)?"SI":"NO"), "Preparar efectos     " + std::string(prepare==2?"AL INICIAR":"AL USAR"), "Restaurar rendimiento"}};
                for (int i=0;i<4;++i) ink(image,(i==videoRow?"> ":"  ")+rows[i],36,138+i*42,22,i==videoRow?highlight:nativeText);
                ink(image,status,28,342,16,nativeText,460);
            }
            else
            {
                ink(image,"ACCION",28,119,16,nativeText); ink(image,"TECLA 1      TECLA 2      TECLA 3      MANDO",168,119,14,nativeText);
                const int first = std::clamp(view.row - 4,0,std::max(0,int(view.rows.size())-8));
                for(int i=0;i<8 && first+i<int(view.rows.size());++i)
                {
                    const int row=first+i,y=146+i*24; const bool selected=row==view.row;
                    ink(image,(selected?"> ":"  ")+view.rows[row][0],22,y,17,selected?highlight:nativeText,137);
                    for(int c=0;c<4;++c)
                    {
                        const bool cursor=selected && c==view.column;
                        const std::string value=cursor && view.capturing?"...":view.rows[row][c+1];
                        ink(image,(cursor?"[":"")+value+(cursor?"]":""),163+c*82,y,15,cursor?highlight:nativeText,80);
                    }
                }
                ink(image,"ENTER / A: cambiar   DEL / X: borrar",28,351,16,nativeText);
                ink(image,view.capturing?"Pulsa una tecla o boton. ESC: cancelar":view.status,28,374,15,highlight,460);
            }
            ink(image,"TAB / R1: controles - video   ESC / B: volver",28,411,15,nativeText);
            auto next=std::make_shared<Published>(); static uint64_t serial=0; next->version=++serial;
            next->title=title.encode(image); next->ingame=ingame.encode(image);
            std::lock_guard<std::mutex> lock(publicationMutex); publication=std::move(next);
        }

        void nativeDraw(uint8_t *ram, R5900Context *ctx, PS2Runtime *, bool isTitle)
        {
            auto &asset = isTitle ? title : ingame; if (asset.pixels.empty()) return;
            const uint32_t object = GPR_U32(ctx,4);
            if (object >= 0x2000000-0x200 || ram[object+0x5a] < 1 || ram[object+0x5a] > 3) return;
            std::shared_ptr<const Published> built;
            { std::lock_guard<std::mutex> lock(publicationMutex); built=publication; }
            if (!built) return;
            if (!asset.address || asset.address + asset.file.size() > 0x2000000 || std::memcmp(ram+asset.address,asset.file.data(),24))
            {
                asset.address=0; asset.applied=0;
                for(uint32_t a=0x100000;a+asset.file.size()<=0x2000000;a+=16)
                    if(!std::memcmp(ram+a,asset.file.data(),24)) { asset.address=a; break; }
            }
            const auto &data=isTitle?built->title:built->ingame;
            // Loading a state can restore the same resource address with its original payload.
            if (asset.address && (asset.applied != built->version || std::memcmp(ram+asset.address+asset.record+0xe0,data.data(),data.size())))
            {
                std::memcpy(ram+asset.address+asset.record+0xe0,data.data(),data.size()); asset.applied=built->version;
                std::fprintf(stderr,"[native-options] IDD %s payload v%llu at %08x\n",isTitle?"title":"game",(unsigned long long)built->version,asset.address);
            }
        }
        void nativeControl(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
        {
            if (title.pixels.empty()) return;
            const uint32_t object=GPR_U32(ctx,4); if(object>=0x2000000-0x200) return;
            const int phase=int8_t(ram[object+0x5a]);
            if (phase==2)
            {
                nativeActive=true;
                uint64_t input=leaveNative.exchange(false)?0x20000000ull:0;
                std::memcpy(ram+0x747640,&input,8); std::memset(ram+0x747650,0,8);
                if(input) nativeActive=false;
            }
            else nativeActive=false;
        }
        void shutdown()
        {
#ifdef GODHAND_WITH_EXTRAS
            gh_extras::shutdownMenu();
#endif
            ps2x::controls::setFont(nullptr);
            if(startupBackground.id) UnloadTexture(startupBackground);
            if(startupWindow.id) UnloadTexture(startupWindow);
            if(owned) UnloadFont(font);
            font={}; owned=false;
        }
    }
    void install(const std::filesystem::path &path, bool)
    {
        settings=path;
        if(const char *test=std::getenv("GODHAND_UI_SETTINGS_TEST_FILE")) settings=test;
#ifdef GODHAND_WITH_EXTRAS
        ps2x::hostui::install(initialize,startup,draw,shutdown,gh_extras::tickMenu);
#else
        ps2x::hostui::install(initialize,startup,draw,shutdown);
#endif
    }
    void installNativeHooks(PS2Runtime &)
    {
        auto &hooks=FunctionHooks::instance();
        hooks.observeEntry(0x167908,"native_title_controls",nativeControl);
        hooks.observeEntry(0x166bb8,"native_game_controls",nativeControl);
        hooks.observeEntry(0x1678b8,"native_title_controls_resource",[](uint8_t *r,R5900Context *c,PS2Runtime *rt){nativeDraw(r,c,rt,true);});
        hooks.observeEntry(0x166b60,"native_game_controls_resource",[](uint8_t *r,R5900Context *c,PS2Runtime *rt){nativeDraw(r,c,rt,false);});
    }
}

