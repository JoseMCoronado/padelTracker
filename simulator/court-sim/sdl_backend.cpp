#define SDL_MAIN_HANDLED
#include "sdl_backend.hpp"

#include <SDL.h>

#include "lvgl.h"

namespace padel::sim {
namespace {

SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;

lv_disp_draw_buf_t g_draw_buf;
lv_disp_drv_t g_disp_drv;
lv_indev_drv_t g_mouse_drv;
std::vector<lv_color_t> g_pixels;

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    const SDL_Rect rect{area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area)};
    SDL_UpdateTexture(g_texture, &rect, color_p,
                      lv_area_get_width(area) * static_cast<int>(sizeof(lv_color_t)));
    if (lv_disp_flush_is_last(drv)) {
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
        SDL_RenderPresent(g_renderer);
    }
    lv_disp_flush_ready(drv);
}

void mouse_read_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
    int x = 0;
    int y = 0;
    const std::uint32_t buttons = SDL_GetMouseState(&x, &y);
    data->point.x = static_cast<lv_coord_t>(x);
    data->point.y = static_cast<lv_coord_t>(y);
    data->state = (buttons & SDL_BUTTON_LMASK) != 0 ? LV_INDEV_STATE_PRESSED
                                                    : LV_INDEV_STATE_RELEASED;
}

}  // namespace

bool SdlBackend::init(int width, int height, const char* title) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_Window* window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          width, height, SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        return false;
    }
    g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (g_renderer == nullptr) {
        g_renderer = SDL_CreateRenderer(window, -1, 0);
    }
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);

    lv_init();
    g_pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    lv_disp_draw_buf_init(&g_draw_buf, g_pixels.data(), nullptr, g_pixels.size());
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res = static_cast<lv_coord_t>(width);
    g_disp_drv.ver_res = static_cast<lv_coord_t>(height);
    g_disp_drv.flush_cb = flush_cb;
    g_disp_drv.draw_buf = &g_draw_buf;
    lv_disp_drv_register(&g_disp_drv);

    lv_indev_drv_init(&g_mouse_drv);
    g_mouse_drv.type = LV_INDEV_TYPE_POINTER;
    g_mouse_drv.read_cb = mouse_read_cb;
    lv_indev_drv_register(&g_mouse_drv);

    window_ = window;
    renderer_ = g_renderer;
    texture_ = g_texture;
    last_tick_ms_ = SDL_GetTicks();
    return true;
}

void SdlBackend::shutdown() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    }
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
    }
    SDL_Quit();
}

bool SdlBackend::screenshot(const char* path) {
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(g_renderer, &width, &height);
    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return false;
    }
    // Repaint from the LVGL texture so we read a complete frame.
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    const bool read_ok = SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                              surface->pixels, surface->pitch) == 0;
    SDL_RenderPresent(g_renderer);
    // 24-bit BMP keeps the file readable by stock image tools.
    SDL_Surface* rgb = read_ok ? SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGB24, 0)
                               : nullptr;
    const bool saved = rgb != nullptr && SDL_SaveBMP(rgb, path) == 0;
    if (rgb != nullptr) {
        SDL_FreeSurface(rgb);
    }
    SDL_FreeSurface(surface);
    return saved;
}

bool SdlBackend::pump(std::vector<KeyEvent>& keys_out) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            return false;
        }
        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            KeyEvent key{};
            key.keycode = event.key.keysym.sym;
            key.shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
            keys_out.push_back(key);
        }
    }
    const std::uint32_t now = SDL_GetTicks();
    lv_tick_inc(now - last_tick_ms_);
    last_tick_ms_ = now;
    return true;
}

}  // namespace padel::sim
