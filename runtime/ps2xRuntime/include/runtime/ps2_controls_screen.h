#pragma once

// Player 1 input capture and persistence, on the window thread. Ports can use nativePresentation/nativeView to draw
// these bindings inside a guest menu instead of using the default host overlay. While editing, guest pads are idle;
// closing saves the controls file and holds input until the physical controls are released.
struct Font;
#include <array>
#include <string>
#include <vector>
namespace ps2x::controls
{
    struct NativeView { int row = 0, column = 0; bool capturing = false; std::string status; std::vector<std::array<std::string, 5>> rows; };
    void setNativePresentation(bool enabled);
    void setNativePageInput(bool enabled);
    NativeView nativeView();
    void saveAndClose();
    void requestOpen();
    void setFont(const Font *font);
    [[nodiscard]] bool isOpen() noexcept;
    // Once per host frame, before ps2_host_input::sample(). drawable = false (PCSX2's GS covers this window): the
    // screen does not open, so input is never blocked behind an invisible screen.
    void hostTick(bool drawable);
    // Inside BeginDrawing, after the game image.
    void draw();
    // After the run loop (window closed or the run stopped): closes the screen, saving unsaved edits.
    void shutdown();
}
