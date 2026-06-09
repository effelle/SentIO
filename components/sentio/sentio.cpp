#include "sentio.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"

// LittleFS — Arduino framework only (IDF path is a Phase 4 enhancement)
#ifdef USE_ARDUINO
#  include "FS.h"
#  include "LittleFS.h"
#endif

namespace esphome {
namespace sentio {

static const char *const TAG = "sentio";

// Swipe threshold in pixels before a drag is classified as a swipe
static constexpr int16_t SWIPE_THRESHOLD = 30;

// ---------------------------------------------------------------------------
// Static LVGL callbacks (must be free functions — no captures allowed)
// ---------------------------------------------------------------------------

// Called for every interactive widget event (click, value change, etc.)
static void sentio_widget_event_cb(lv_event_t *e) {
  auto *data = static_cast<WidgetEventData *>(lv_event_get_user_data(e));
  if (data != nullptr) {
    data->component->handle_widget_event(e, data->widget_id);
  }
}

// Frees the WidgetEventData struct when the widget is deleted by LVGL
static void sentio_widget_delete_cb(lv_event_t *e) {
  auto *data = static_cast<WidgetEventData *>(lv_event_get_user_data(e));
  delete data;
}

// Called by the shield overlay when the user taps to wake the screen
static void sentio_shield_event_cb(lv_event_t *e) {
  auto *self = static_cast<SentioComponent *>(lv_event_get_user_data(e));
  if (self != nullptr) {
    self->wake_up();
  }
}

// Burn-in protection timer callback — shifts root_container_ by ±1 px
static void sentio_burn_in_cb(lv_timer_t *timer) {
  auto *self = static_cast<SentioComponent *>(lv_timer_get_user_data(timer));
  if (self != nullptr) {
    self->do_burn_in_shift();
  }
}

// Called by the root container when a swipe gesture is detected
static void sentio_gesture_event_cb(lv_event_t *e) {
  auto *self = static_cast<SentioComponent *>(lv_event_get_user_data(e));
  if (self != nullptr) {
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev != nullptr) {
      lv_dir_t dir = lv_indev_get_gesture_dir(indev);
      self->handle_gesture(dir);
    }
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SentioComponent::setup() {
  ESP_LOGI(TAG, "SentIO setup — LVGL overlay starting");

  last_activity_ms_ = millis();

  // Create the root container that all SentIO widgets live in.
  // This lets us shift the entire UI for burn-in protection without touching
  // native LVGL screen objects.
  root_container_ = lv_obj_create(lv_scr_act());
  lv_obj_set_size(root_container_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(root_container_, 0, 0);
  lv_obj_set_style_bg_opa(root_container_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root_container_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(root_container_, 0, LV_PART_MAIN);
  lv_obj_clear_flag(root_container_, LV_OBJ_FLAG_SCROLLABLE);

  // Register swipe gesture handler on the root container
  lv_obj_add_event_cb(root_container_, sentio_gesture_event_cb, LV_EVENT_GESTURE, this);

  // Register HA API services
  register_service(&SentioComponent::service_run_jsonl,
                   "sentio_run_jsonl", {"line"});
  register_service(&SentioComponent::service_clear,
                   "sentio_clear", {});
  register_service(&SentioComponent::service_load_layout,
                   "sentio_load_layout", {"filename"});
  register_service(&SentioComponent::service_save_layout_line,
                   "sentio_save_layout_line", {"line", "append"});
  register_service(&SentioComponent::service_bind_sensor,
                   "sentio_bind_sensor", {"widget_id", "sensor_id", "format"});
  register_service(&SentioComponent::service_load_page,
                   "sentio_load_page", {"page_id"});

  // Burn-in protection timer (fires every 60 seconds)
  if (anti_burn_in_) {
    burn_in_timer_ = lv_timer_create(sentio_burn_in_cb, 60000, this);
  }

  // Load startup layout from LittleFS if configured
  if (!startup_layout_.empty()) {
    load_layout_from_file(startup_layout_);
  }

  ESP_LOGD(TAG, "SentIO setup complete (sleep_timeout=%u ms, anti_burn_in=%s)",
           sleep_timeout_ms_, anti_burn_in_ ? "true" : "false");
}

void SentioComponent::loop() {
  uint32_t now = millis();

  // ── Track touch activity from upstream driver ────────────────────────────
  if (touch_source_ != nullptr && !touch_source_->get_touches().empty()) {
    if (is_sleeping_) {
      // Touch while sleeping: wake up, optionally suppress this first event
      wake_up();
      if (suppress_wake_click_) {
        ignore_wake_tap_ = true;
        return; // Swallow this frame
      }
    }
    last_activity_ms_ = now;
  }

  // ── Sleep timeout ────────────────────────────────────────────────────────
  if (!is_sleeping_ && sleep_timeout_ms_ > 0) {
    if (now - last_activity_ms_ > sleep_timeout_ms_) {
      enter_sleep();
    }
  }
}

void SentioComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SentIO:");
  ESP_LOGCONFIG(TAG, "  Sleep timeout : %u ms", sleep_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Suppress wake : %s", suppress_wake_click_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Anti burn-in  : %s", anti_burn_in_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Disp off/sleep: %s", display_off_before_sleep_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Touch source  : %s", touch_source_ ? "configured" : "none");
  ESP_LOGCONFIG(TAG, "  Backlight     : %s",
                backlight_light_ ? "light" : (backlight_output_ ? "output" : "none"));
  if (!startup_layout_.empty()) {
    ESP_LOGCONFIG(TAG, "  Startup layout: %s", startup_layout_.c_str());
  }
}

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------

void SentioComponent::enter_sleep() {
  if (is_sleeping_) return;
  is_sleeping_ = true;

  ESP_LOGI(TAG, "Entering sleep");

  // Optionally black out the screen before the on_sleep trigger fires.
  // This prevents display glow when SPI bus floats during deep sleep
  // (important for boards like JC3248W535C where CS/DC pins are exposed).
  if (display_off_before_sleep_) {
    lv_obj_set_style_opa(root_container_, LV_OPA_TRANSP, LV_PART_MAIN);
    set_backlight(0.0f);
  }

  // Pause all LVGL timers — stops animations from running on a dark screen
  // and reduces ESP32 CPU load significantly while sleeping.
  lv_timer_pause(burn_in_timer_); // Safe even if null
  // Note: lv_timer_pause_all() pauses ALL timers including LVGL internals.
  // We pause only our own timer here; the user can pause more via on_sleep.

  // Spawn the touch shield so the first wake tap is intercepted safely
  spawn_shield();

  // Fire the on_sleep automation trigger — user YAML decides what happens
  // (deep_sleep.enter, light.turn_off, nothing, etc.)
  on_sleep_callbacks_.call();
}

void SentioComponent::wake_up() {
  if (!is_sleeping_) return;
  is_sleeping_ = false;
  last_activity_ms_ = millis();

  ESP_LOGI(TAG, "Waking up");

  // Remove the touch-absorbing shield
  destroy_shield();

  // Restore the root container opacity
  lv_obj_set_style_opa(root_container_, LV_OPA_COVER, LV_PART_MAIN);

  // Restore backlight to full brightness
  set_backlight(1.0f);

  // Resume our burn-in timer if it was active
  if (anti_burn_in_ && burn_in_timer_ != nullptr) {
    lv_timer_resume(burn_in_timer_);
  }

  on_wake_callbacks_.call();
}

void SentioComponent::set_backlight(float level) {
  if (backlight_light_ != nullptr) {
    auto call = backlight_light_->make_call();
    call.set_brightness(level);
    call.set_state(level > 0.0f);
    call.perform();
  } else if (backlight_output_ != nullptr) {
    backlight_output_->set_level(level);
  }
}

// ---------------------------------------------------------------------------
// Wake-up shield — full-screen transparent overlay
// ---------------------------------------------------------------------------

void SentioComponent::spawn_shield() {
  if (shield_ != nullptr) return; // Already active

  shield_ = lv_obj_create(lv_layer_top()); // Top layer — above all content
  lv_obj_set_size(shield_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(shield_, 0, 0);
  lv_obj_set_style_bg_opa(shield_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(shield_, 0, LV_PART_MAIN);
  lv_obj_add_flag(shield_, LV_OBJ_FLAG_CLICKABLE);

  // A single tap on the shield wakes the screen; the shield then destroys itself
  lv_obj_add_event_cb(shield_, sentio_shield_event_cb, LV_EVENT_CLICKED, this);
}

void SentioComponent::destroy_shield() {
  if (shield_ == nullptr) return;
  lv_obj_del(shield_);
  shield_ = nullptr;
}

// ---------------------------------------------------------------------------
// Burn-in protection
// ---------------------------------------------------------------------------

void SentioComponent::do_burn_in_shift() {
  if (root_container_ == nullptr || is_sleeping_) return;

  lv_coord_t x = lv_obj_get_x(root_container_);
  lv_coord_t y = lv_obj_get_y(root_container_);

  x += burn_in_dx_;
  y += burn_in_dy_;

  // Bounce at ±2 px to avoid visible edge clipping
  if (x >= 2 || x <= -2) burn_in_dx_ = -burn_in_dx_;
  if (y >= 2 || y <= -2) burn_in_dy_ = -burn_in_dy_;

  lv_obj_set_pos(root_container_, x, y);
}

// ---------------------------------------------------------------------------
// HA API services
// ---------------------------------------------------------------------------

void SentioComponent::service_run_jsonl(std::string line) {
  if (line.empty()) return;
  parse_jsonl_line(line);
  // Any service call resets the idle timer
  last_activity_ms_ = millis();
}

void SentioComponent::service_clear() {
  ESP_LOGD(TAG, "sentio_clear: removing %zu widgets", widgets_.size());
  for (auto &kv : widgets_) {
    if (kv.second != nullptr) {
      lv_obj_del(kv.second);
    }
  }
  widgets_.clear();
}

void SentioComponent::service_load_layout(std::string filename) {
  load_layout_from_file(filename);
}

void SentioComponent::service_save_layout_line(std::string line, bool append) {
#ifdef USE_ARDUINO
  if (!LittleFS.begin(false)) {
    ESP_LOGE(TAG, "LittleFS mount failed — cannot save layout line");
    return;
  }
  const char *mode = append ? "a" : "w";
  File f = LittleFS.open(startup_layout_.empty() ? "/sentio.jsonl" : startup_layout_.c_str(), mode);
  if (!f) {
    ESP_LOGE(TAG, "Cannot open layout file for writing");
    return;
  }
  f.println(line.c_str());
  f.close();
#else
  ESP_LOGW(TAG, "save_layout_line: LittleFS write not yet implemented for IDF framework");
#endif
}

// ---------------------------------------------------------------------------
// JSONL parser
// ---------------------------------------------------------------------------

void SentioComponent::parse_jsonl_line(const std::string &line) {
  bool ok = json::parse_json(line, [&](JsonObject root) -> bool {
    // Every valid line must have an "id" field
    if (!root.containsKey("id")) {
      ESP_LOGW(TAG, "JSONL line missing 'id' field: %s", line.c_str());
      return false;
    }

    std::string id = root["id"].as<std::string>();

    // Delete command: {"id": "my_widget", "delete": true}
    if (root.containsKey("delete") && root["delete"].as<bool>()) {
      auto it = widgets_.find(id);
      if (it != widgets_.end()) {
        lv_obj_del(it->second);
        widgets_.erase(it);
        ESP_LOGD(TAG, "Deleted widget '%s'", id.c_str());
      }
      return true;
    }

    lv_obj_t *obj = nullptr;

    if (root.containsKey("obj")) {
      // Creation mode: widget does not yet exist (or we replace it)
      std::string type = root["obj"].as<std::string>();
      auto it = widgets_.find(id);
      if (it != widgets_.end()) {
        // Replace existing widget
        lv_obj_del(it->second);
        widgets_.erase(it);
      }
      obj = create_widget(type, root_container_);
      if (obj == nullptr) {
        ESP_LOGW(TAG, "Unknown widget type '%s'", type.c_str());
        return false;
      }
      widgets_[id] = obj;
      register_widget_events(obj, id);
      ESP_LOGD(TAG, "Created widget '%s' (type=%s)", id.c_str(), type.c_str());
    } else {
      // Update mode: widget must already exist
      auto it = widgets_.find(id);
      if (it == widgets_.end()) {
        ESP_LOGW(TAG, "Widget '%s' not found for update", id.c_str());
        return false;
      }
      obj = it->second;
    }

    apply_properties(obj, root);
    return true;
  });

  if (!ok) {
    ESP_LOGW(TAG, "Failed to parse JSONL: %s", line.c_str());
  }
}

// ---------------------------------------------------------------------------
// Widget factory — LVGL v9 API names
// ---------------------------------------------------------------------------

lv_obj_t *SentioComponent::create_widget(const std::string &type, lv_obj_t *parent) {
  if (type == "obj" || type == "container") return lv_obj_create(parent);
  if (type == "label")                      return lv_label_create(parent);
  if (type == "button" || type == "btn")    return lv_button_create(parent);  // LVGL v9
  if (type == "slider")                     return lv_slider_create(parent);
#ifdef LV_USE_SWITCH
  if (type == "switch" || type == "sw")     return lv_switch_create(parent);
#endif
  if (type == "checkbox" || type == "cb")   return lv_checkbox_create(parent);
#ifdef LV_USE_DROPDOWN
  if (type == "dropdown" || type == "dd")   return lv_dropdown_create(parent);
#endif
#ifdef LV_USE_TEXTAREA
  if (type == "textarea" || type == "ta")   return lv_textarea_create(parent);
#endif
  if (type == "arc")                        return lv_arc_create(parent);
  if (type == "bar")                        return lv_bar_create(parent);
  if (type == "image" || type == "img")     return lv_image_create(parent);   // LVGL v9
  if (type == "spinner")                    return lv_spinner_create(parent);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Property applicator
// ---------------------------------------------------------------------------

void SentioComponent::apply_properties(lv_obj_t *obj, JsonObject props) {
  // ── Position & size ────────────────────────────────────────────────────
  if (props.containsKey("x")) lv_obj_set_x(obj, props["x"].as<int>());
  if (props.containsKey("y")) lv_obj_set_y(obj, props["y"].as<int>());
  if (props.containsKey("w") || props.containsKey("width")) {
    int w = props.containsKey("w") ? props["w"].as<int>() : props["width"].as<int>();
    lv_obj_set_width(obj, w);
  }
  if (props.containsKey("h") || props.containsKey("height")) {
    int h = props.containsKey("h") ? props["h"].as<int>() : props["height"].as<int>();
    lv_obj_set_height(obj, h);
  }

  // ── Alignment ──────────────────────────────────────────────────────────
  if (props.containsKey("align")) {
    const char *a = props["align"].as<const char *>();
    lv_align_t align = LV_ALIGN_DEFAULT;
    if      (strcmp(a, "center")       == 0) align = LV_ALIGN_CENTER;
    else if (strcmp(a, "top_left")     == 0) align = LV_ALIGN_TOP_LEFT;
    else if (strcmp(a, "top_mid")      == 0) align = LV_ALIGN_TOP_MID;
    else if (strcmp(a, "top_right")    == 0) align = LV_ALIGN_TOP_RIGHT;
    else if (strcmp(a, "bottom_left")  == 0) align = LV_ALIGN_BOTTOM_LEFT;
    else if (strcmp(a, "bottom_mid")   == 0) align = LV_ALIGN_BOTTOM_MID;
    else if (strcmp(a, "bottom_right") == 0) align = LV_ALIGN_BOTTOM_RIGHT;
    else if (strcmp(a, "left_mid")     == 0) align = LV_ALIGN_LEFT_MID;
    else if (strcmp(a, "right_mid")    == 0) align = LV_ALIGN_RIGHT_MID;
    lv_obj_set_align(obj, align);
  }

  // ── Text content ───────────────────────────────────────────────────────
  if (props.containsKey("text")) {
    const char *text = props["text"].as<const char *>();
    if (lv_obj_check_type(obj, &lv_label_class)) {
      lv_label_set_text(obj, text);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
      lv_checkbox_set_text(obj, text);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
      // Auto-create a child label on the button if text is provided
      lv_obj_t *existing_label = lv_obj_get_child(obj, 0);
      if (existing_label == nullptr || !lv_obj_check_type(existing_label, &lv_label_class)) {
        existing_label = lv_label_create(obj);
        lv_obj_center(existing_label);
      }
      lv_label_set_text(existing_label, text);
    }
  }

  // ── Numeric value (sliders, arcs, bars) ───────────────────────────────
  if (props.containsKey("value")) {
    int val = props["value"].as<int>();
    if      (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_value(obj, val, LV_ANIM_OFF);
    else if (lv_obj_check_type(obj, &lv_arc_class))    lv_arc_set_value(obj, val);
    else if (lv_obj_check_type(obj, &lv_bar_class))    lv_bar_set_value(obj, val, LV_ANIM_OFF);
  }

  // ── Range (min/max for sliders, arcs) ─────────────────────────────────
  if (props.containsKey("min") || props.containsKey("max")) {
    int mn = props.containsKey("min") ? props["min"].as<int>() : 0;
    int mx = props.containsKey("max") ? props["max"].as<int>() : 100;
    if      (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_range(obj, mn, mx);
    else if (lv_obj_check_type(obj, &lv_arc_class))    lv_arc_set_range(obj, mn, mx);
    else if (lv_obj_check_type(obj, &lv_bar_class))    lv_bar_set_range(obj, mn, mx);
  }

  // ── Checked state ──────────────────────────────────────────────────────
  if (props.containsKey("checked")) {
    if (props["checked"].as<bool>()) {
      lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(obj, LV_STATE_CHECKED);
    }
  }

  // ── Visibility ─────────────────────────────────────────────────────────
  if (props.containsKey("hidden")) {
    if (props["hidden"].as<bool>()) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // ── Scrollable ─────────────────────────────────────────────────────────
  if (props.containsKey("scrollable")) {
    if (props["scrollable"].as<bool>()) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else {
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
  }

  // ── Styles — colors ────────────────────────────────────────────────────
  if (props.containsKey("bg_color")) {
    lv_color_t c = parse_hex_color(props["bg_color"].as<const char *>());
    lv_style_selector_t sel = (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, c, sel);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, sel);
  }
  if (props.containsKey("text_color")) {
    lv_color_t c = parse_hex_color(props["text_color"].as<const char *>());
    lv_obj_set_style_text_color(obj, c, (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_DEFAULT));
  }
  if (props.containsKey("border_color")) {
    lv_color_t c = parse_hex_color(props["border_color"].as<const char *>());
    lv_obj_set_style_border_color(obj, c, (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_DEFAULT));
  }

  // ── Styles — geometry ──────────────────────────────────────────────────
  if (props.containsKey("radius")) {
    lv_obj_set_style_radius(obj, props["radius"].as<int>(), LV_PART_MAIN);
  }
  if (props.containsKey("border_width")) {
    lv_obj_set_style_border_width(obj, props["border_width"].as<int>(), LV_PART_MAIN);
  }
  if (props.containsKey("pad")) {
    lv_obj_set_style_pad_all(obj, props["pad"].as<int>(), LV_PART_MAIN);
  }
  if (props.containsKey("pad_top"))    lv_obj_set_style_pad_top(obj,    props["pad_top"].as<int>(),    LV_PART_MAIN);
  if (props.containsKey("pad_bottom")) lv_obj_set_style_pad_bottom(obj, props["pad_bottom"].as<int>(), LV_PART_MAIN);
  if (props.containsKey("pad_left"))   lv_obj_set_style_pad_left(obj,   props["pad_left"].as<int>(),   LV_PART_MAIN);
  if (props.containsKey("pad_right"))  lv_obj_set_style_pad_right(obj,  props["pad_right"].as<int>(),  LV_PART_MAIN);

  // ── Opacity ────────────────────────────────────────────────────────────
  if (props.containsKey("opacity") || props.containsKey("opa")) {
    int opa = props.containsKey("opacity") ? props["opacity"].as<int>() : props["opa"].as<int>();
    lv_obj_set_style_opa(obj, (lv_opa_t)opa, LV_PART_MAIN);
  }

  // ── Image source path ──────────────────────────────────────────────────
  if (props.containsKey("src") && lv_obj_check_type(obj, &lv_image_class)) {
    lv_image_set_src(obj, props["src"].as<const char *>());
  }

  // ── Dropdown options ───────────────────────────────────────────────────
#ifdef LV_USE_DROPDOWN
  if (props.containsKey("options") && lv_obj_check_type(obj, &lv_dropdown_class)) {
    lv_dropdown_set_options(obj, props["options"].as<const char *>());
  }
#endif
}

// ---------------------------------------------------------------------------
// Event registration
// ---------------------------------------------------------------------------

void SentioComponent::register_widget_events(lv_obj_t *obj, const std::string &id) {
  auto *data = new WidgetEventData{this, id};

  // Use LV_EVENT_CLICKED (fires on release within bounds) rather than
  // LV_EVENT_PRESSED (fires on touch-down) to prevent drag/swipe collisions.
  lv_obj_add_event_cb(obj, sentio_widget_event_cb, LV_EVENT_CLICKED, data);

  // Sliders and switches also send value-changed events
  lv_obj_add_event_cb(obj, sentio_widget_event_cb, LV_EVENT_VALUE_CHANGED, data);

  // Clean up heap-allocated data when the widget is deleted
  lv_obj_add_event_cb(obj, sentio_widget_delete_cb, LV_EVENT_DELETE, data);
}

// ---------------------------------------------------------------------------
// Widget event handler — fires Home Assistant events
// ---------------------------------------------------------------------------

void SentioComponent::handle_widget_event(lv_event_t *e, const std::string &widget_id) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *obj        = static_cast<lv_obj_t *>(lv_event_get_target(e));

  std::string event_type;
  std::string value_str;

  if (code == LV_EVENT_CLICKED) {
    event_type = "clicked";
    // Report checked state for toggleable widgets
    if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
      value_str = "true";
    } else {
      value_str = "false";
    }
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    event_type = "value_changed";
    if      (lv_obj_check_type(obj, &lv_slider_class)) {
      value_str = to_string(lv_slider_get_value(obj));
    } else if (lv_obj_check_type(obj, &lv_arc_class)) {
      value_str = to_string(lv_arc_get_value(obj));
#ifdef LV_USE_SWITCH
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
      value_str = lv_obj_has_state(obj, LV_STATE_CHECKED) ? "true" : "false";
#endif
#ifdef LV_USE_DROPDOWN
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
      char buf[64] = {0};
      lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
      value_str = buf;
#endif
    }
  } else {
    return; // Not an event we report
  }

  // Fire Home Assistant event: esphome.sentio_event
  // Guarded: requires homeassistant_services: true in api: section, OR
  // USE_API_HOMEASSISTANT_SERVICES injected by __init__.py via cg.add_define.
#ifdef USE_API_HOMEASSISTANT_SERVICES
  fire_homeassistant_event("esphome.sentio_event", {
    {"widget_id", widget_id},
    {"event",     event_type},
    {"value",     value_str},
  });
#else
  ESP_LOGD(TAG, "Widget event [%s] %s=%s (enable homeassistant_services in api: for HA events)",
           widget_id.c_str(), event_type.c_str(), value_str.c_str());
#endif

  // Reset idle timer on any widget interaction
  last_activity_ms_ = millis();
}

// ---------------------------------------------------------------------------
// LittleFS layout loader
// ---------------------------------------------------------------------------

void SentioComponent::load_layout_from_file(const std::string &path) {
#ifdef USE_ARDUINO
  if (!LittleFS.begin(false)) {
    ESP_LOGE(TAG, "LittleFS mount failed — cannot load '%s'", path.c_str());
    return;
  }
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    ESP_LOGW(TAG, "Layout file not found: %s", path.c_str());
    return;
  }
  ESP_LOGI(TAG, "Loading layout from %s", path.c_str());
  while (f.available()) {
    String raw_line = f.readStringUntil('\n');
    raw_line.trim();
    if (raw_line.length() > 2) { // Ignore blank or empty-object lines
      parse_jsonl_line(raw_line.c_str());
    }
  }
  f.close();
#else
  ESP_LOGW(TAG, "load_layout_from_file: IDF LittleFS not yet implemented");
#endif
}

// ---------------------------------------------------------------------------
// Color helper — parses "#RRGGBB" or "RRGGBB" hex strings
// ---------------------------------------------------------------------------

lv_color_t SentioComponent::parse_hex_color(const char *hex) {
  if (hex == nullptr || *hex == '\0') return lv_color_black();
  const char *p = (*hex == '#') ? hex + 1 : hex;
  uint32_t val = (uint32_t)strtoul(p, nullptr, 16);
  return lv_color_hex(val);
}

void SentioComponent::handle_gesture(lv_dir_t dir) {
  std::string gesture_name;
  if (dir == LV_DIR_LEFT) {
    gesture_name = "swipe_left";
    ESP_LOGD(TAG, "Gesture detected: swipe_left");
    this->on_swipe_left_callbacks_.call();
  } else if (dir == LV_DIR_RIGHT) {
    gesture_name = "swipe_right";
    ESP_LOGD(TAG, "Gesture detected: swipe_right");
    this->on_swipe_right_callbacks_.call();
  } else if (dir == LV_DIR_TOP) {
    gesture_name = "swipe_up";
    ESP_LOGD(TAG, "Gesture detected: swipe_up");
    this->on_swipe_up_callbacks_.call();
  } else if (dir == LV_DIR_BOTTOM) {
    gesture_name = "swipe_down";
    ESP_LOGD(TAG, "Gesture detected: swipe_down");
    this->on_swipe_down_callbacks_.call();
  } else {
    return;
  }

  // Also fire Home Assistant event for integration
  fire_homeassistant_event("esphome.sentio_gesture", {
    {"direction", gesture_name}
  });

  // Reset idle timer on any user gesture
  last_activity_ms_ = millis();
}

#ifdef USE_SENSOR
void SentioComponent::register_local_sensor(const std::string &name, sensor::Sensor *sensor) {
  if (sensor != nullptr) {
    this->local_sensors_[name] = sensor;
    ESP_LOGD(TAG, "Registered local sensor '%s'", name.c_str());
  }
}
#endif

void SentioComponent::bind_sensor(const std::string &widget_id, const std::string &sensor_id, const std::string &format) {
#ifdef USE_SENSOR
  auto it = this->local_sensors_.find(sensor_id);
  if (it == this->local_sensors_.end()) {
    ESP_LOGW(TAG, "bind_sensor: local sensor '%s' not found", sensor_id.c_str());
    return;
  }

  sensor::Sensor *sensor = it->second;
  ESP_LOGD(TAG, "Binding sensor '%s' to widget '%s' with format '%s'", sensor_id.c_str(), widget_id.c_str(), format.c_str());

  // Subscribe to sensor state changes
  sensor->add_on_state_callback([this, widget_id, format](float state) {
    // Check if the widget still exists before updating it
    auto wit = this->widgets_.find(widget_id);
    if (wit != this->widgets_.end() && wit->second != nullptr) {
      lv_obj_t *obj = wit->second;
      char buf[64];
      snprintf(buf, sizeof(buf), format.c_str(), state);
      
      if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_label_set_text(obj, buf);
      } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_checkbox_set_text(obj, buf);
      } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_t *existing_label = lv_obj_get_child(obj, 0);
        if (existing_label == nullptr || !lv_obj_check_type(existing_label, &lv_label_class)) {
          existing_label = lv_label_create(obj);
          lv_obj_center(existing_label);
        }
        lv_label_set_text(existing_label, buf);
      }
    }
  });

  // Also apply the current sensor state immediately if it has one
  if (sensor->has_state()) {
    float state = sensor->state;
    auto wit = this->widgets_.find(widget_id);
    if (wit != this->widgets_.end() && wit->second != nullptr) {
      lv_obj_t *obj = wit->second;
      char buf[64];
      snprintf(buf, sizeof(buf), format.c_str(), state);
      
      if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_label_set_text(obj, buf);
      } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_checkbox_set_text(obj, buf);
      } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_t *existing_label = lv_obj_get_child(obj, 0);
        if (existing_label == nullptr || !lv_obj_check_type(existing_label, &lv_label_class)) {
          existing_label = lv_label_create(obj);
          lv_obj_center(existing_label);
        }
        lv_label_set_text(existing_label, buf);
      }
    }
  }
#else
  ESP_LOGW(TAG, "bind_sensor: sensor component is not enabled in this build");
#endif
}

void SentioComponent::service_bind_sensor(std::string widget_id, std::string sensor_id, std::string format) {
  this->bind_sensor(widget_id, sensor_id, format);
}

void SentioComponent::service_load_page(std::string page_id) {
  auto it = this->widgets_.find(page_id);
  if (it == this->widgets_.end()) {
    ESP_LOGW(TAG, "service_load_page: page widget '%s' not found", page_id.c_str());
    return;
  }

  lv_obj_t *target_page = it->second;
  
  if (this->active_page_id_ == page_id) {
    return;
  }

  ESP_LOGD(TAG, "Loading page '%s'", page_id.c_str());

  if (!this->active_page_id_.empty()) {
    auto old_it = this->widgets_.find(this->active_page_id_);
    if (old_it != this->widgets_.end() && old_it->second != nullptr) {
      lv_obj_add_flag(old_it->second, LV_OBJ_FLAG_HIDDEN);
    }
    this->on_page_hide_callbacks_.call(this->active_page_id_);
  }

  this->active_page_id_ = page_id;
  lv_obj_clear_flag(target_page, LV_OBJ_FLAG_HIDDEN);
  this->on_page_show_callbacks_.call(page_id);

  last_activity_ms_ = millis();
}

}  // namespace sentio
}  // namespace esphome
