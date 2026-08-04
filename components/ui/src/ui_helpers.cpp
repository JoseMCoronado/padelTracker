#include <cstring>

#include "padel/ui/tokens.hpp"
#include "screens.hpp"

namespace padel::ui::internal {

void set_text(lv_obj_t* label, const std::string& text) {
    if (label != nullptr && text != lv_label_get_text(label)) {
        lv_label_set_text(label, text.c_str());
    }
}

void set_text(lv_obj_t* label, const char* text) {
    if (label != nullptr && std::strcmp(text, lv_label_get_text(label)) != 0) {
        lv_label_set_text(label, text);
    }
}

lv_obj_t* make_screen_root() {
    lv_obj_t* root = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(root, tokens::bg(), 0);
    lv_obj_set_style_text_color(root, tokens::text(), 0);
    lv_obj_set_style_pad_all(root, tokens::kSpaceM, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

lv_obj_t* make_panel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, tokens::surface(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, tokens::kRadius, 0);
    lv_obj_set_style_pad_all(panel, tokens::kSpaceM, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* make_button(lv_obj_t* parent, const char* text, lv_coord_t min_height,
                      lv_event_cb_t handler, void* user_data) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_style_bg_color(button, tokens::surface_raised(), 0);
    lv_obj_set_style_radius(button, tokens::kRadius, 0);
    lv_obj_set_height(button, min_height);
    lv_obj_set_style_pad_hor(button, tokens::kSpaceL, 0);
    if (handler != nullptr) {
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_obj_set_style_text_font(label, tokens::font_heading(), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

}  // namespace padel::ui::internal
