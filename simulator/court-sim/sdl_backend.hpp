#pragma once

#include <cstdint>
#include <vector>

// Minimal SDL2 host for LVGL: window-as-display plus mouse-as-touch. The
// firmware equivalent is the board profile (RGB panel + GT911); everything
// above this line is identical on device and desktop.
namespace padel::sim {

struct KeyEvent {
    int keycode = 0;   // SDLK_*
    bool shift = false;
};

class SdlBackend {
public:
    bool init(int width, int height, const char* title);
    void shutdown();

    // Pumps SDL events, feeds LVGL's tick, and collects app-level hotkeys.
    // Returns false when the window was closed.
    bool pump(std::vector<KeyEvent>& keys_out);

    // Saves the currently presented frame as a BMP (screenshot tour).
    bool screenshot(const char* path);

private:
    void* window_ = nullptr;
    void* renderer_ = nullptr;
    void* texture_ = nullptr;
    std::uint32_t last_tick_ms_ = 0;
};

}  // namespace padel::sim
