#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/automation.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/output/float_output.h"
// json_util is included in sentio.cpp only; ArduinoJson types used here
// are forward-declared via the ArduinoJson header pulled by automation.h.

#include "lvgl.h"
#include <ArduinoJson.h>  // for JsonObject in apply_properties signature

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <string>
#include <unordered_map>
#include <functional>

namespace esphome {
namespace sentio {

// ---------------------------------------------------------------------------
// Touch-state machine (used when a touch_source is configured)
// ---------------------------------------------------------------------------
enum class TouchState : uint8_t {
  IDLE,      // Finger up, waiting
  START,     // Finger just placed, measuring intent
  DRAGGING,  // Movement exceeded threshold — classifying as swipe
};

// ---------------------------------------------------------------------------
// LVGL event user-data bundle (heap-allocated, freed on LV_EVENT_DELETE)
// ---------------------------------------------------------------------------
struct WidgetEventData {
  class SentioComponent *component;
  std::string           widget_id;
};

// ---------------------------------------------------------------------------
// Forward-declare for Trigger constructors
// ---------------------------------------------------------------------------
class SentioComponent;

// ---------------------------------------------------------------------------
// Trigger classes — constructors self-register with parent via callback mgr.
// ---------------------------------------------------------------------------
class SleepTrigger : public Trigger<> {
  public:
   explicit SleepTrigger(SentioComponent *parent);
};

class WakeTrigger : public Trigger<> {
  public:
   explicit WakeTrigger(SentioComponent *parent);
};

class SwipeLeftTrigger : public Trigger<> {
  public:
   explicit SwipeLeftTrigger(SentioComponent *parent);
};

class SwipeRightTrigger : public Trigger<> {
  public:
   explicit SwipeRightTrigger(SentioComponent *parent);
};

class SwipeUpTrigger : public Trigger<> {
  public:
   explicit SwipeUpTrigger(SentioComponent *parent);
};

class SwipeDownTrigger : public Trigger<> {
  public:
   explicit SwipeDownTrigger(SentioComponent *parent);
};

class PageShowTrigger : public Trigger<std::string> {
  public:
   explicit PageShowTrigger(SentioComponent *parent);
};

class PageHideTrigger : public Trigger<std::string> {
  public:
   explicit PageHideTrigger(SentioComponent *parent);
};

// ---------------------------------------------------------------------------
// SentioComponent — main class
// ---------------------------------------------------------------------------
class SentioComponent : public Component, public api::CustomAPIDevice {
 public:
  // ── Lifecycle ─────────────────────────────────────────────────────────────
  void setup()       override;
  void loop()        override;
  void dump_config() override;

  // Run after lvgl component (setup_priority::LATE - 1)
  float get_setup_priority() const override {
    return setup_priority::LATE - 1.0f;
  }

  // ── Setters (called from Python-generated code) ───────────────────────────
  void set_touch_source(touchscreen::Touchscreen *ts) { touch_source_ = ts; }
  void set_backlight_light(light::LightState *bl)     { backlight_light_ = bl; }
  void set_backlight_output(output::FloatOutput *bl)  { backlight_output_ = bl; }
  void set_sleep_timeout(uint32_t ms)                 { sleep_timeout_ms_ = ms; }
  void set_suppress_wake_click(bool v)                { suppress_wake_click_ = v; }
  void set_anti_burn_in(bool v)                       { anti_burn_in_ = v; }
  void set_display_off_before_sleep(bool v)           { display_off_before_sleep_ = v; }
  void set_startup_layout(const std::string &path)    { startup_layout_ = path; }
  void set_soft_sleep_only(bool v)                    { soft_sleep_only_ = v; }

  // ── Trigger callback registration (called by Trigger constructors) ────────
  void add_on_sleep_callback(std::function<void()> cb) {
    on_sleep_callbacks_.add(std::move(cb));
  }
  void add_on_wake_callback(std::function<void()> cb) {
    on_wake_callbacks_.add(std::move(cb));
  }
  void add_on_swipe_left_callback(std::function<void()> cb) {
    on_swipe_left_callbacks_.add(std::move(cb));
  }
  void add_on_swipe_right_callback(std::function<void()> cb) {
    on_swipe_right_callbacks_.add(std::move(cb));
  }
  void add_on_swipe_up_callback(std::function<void()> cb) {
    on_swipe_up_callbacks_.add(std::move(cb));
  }
  void add_on_swipe_down_callback(std::function<void()> cb) {
    on_swipe_down_callbacks_.add(std::move(cb));
  }
  void add_on_page_show_callback(std::function<void(std::string)> cb) {
    on_page_show_callbacks_.add(std::move(cb));
  }
  void add_on_page_hide_callback(std::function<void(std::string)> cb) {
    on_page_hide_callbacks_.add(std::move(cb));
  }

  // ── Public event handler (called from static LVGL callback) ──────────────
  void handle_widget_event(lv_event_t *e, const std::string &widget_id);
  void handle_gesture(lv_dir_t dir);

  // ── LVGL burn-in shift (called from static timer callback) ───────────────
  void do_burn_in_shift();

  // ── wake_up() is public so static shield callback can call it ─────────────
  void wake_up();

  // ── Local sensor registration & binding ───────────────────────────────────
#ifdef USE_SENSOR
  void register_local_sensor(const std::string &name, sensor::Sensor *sensor);
#endif
  void bind_sensor(const std::string &widget_id, const std::string &sensor_id, const std::string &format);

 private:
  // ── Hardware / Component references ───────────────────────────────────────
  touchscreen::Touchscreen *touch_source_{nullptr};
  light::LightState        *backlight_light_{nullptr};
  output::FloatOutput      *backlight_output_{nullptr};

  // ── Power management state ────────────────────────────────────────────────
  uint32_t sleep_timeout_ms_{60000};
  bool     suppress_wake_click_{true};
  bool     anti_burn_in_{false};
  bool     display_off_before_sleep_{true};

  bool     is_sleeping_{false};
  bool     ignore_wake_tap_{false};   // Suppresses first touch event after wake
  bool     soft_sleep_only_{false};   // True when touch chip has no reset_pin
  uint32_t last_activity_ms_{0};

  // ── Touch state machine (Phase 2) ─────────────────────────────────────────
  TouchState touch_state_{TouchState::IDLE};
  int16_t    touch_start_x_{0};
  int16_t    touch_start_y_{0};
  uint32_t   touch_start_ms_{0};

  // ── LVGL objects ──────────────────────────────────────────────────────────
  lv_obj_t  *root_container_{nullptr}; // Full-screen transparent container
  lv_obj_t  *shield_{nullptr};         // Wake-up click interceptor
  lv_timer_t *burn_in_timer_{nullptr};

  // Burn-in shift direction (±1 px, bounces at ±2 px)
  int8_t burn_in_dx_{1};
  int8_t burn_in_dy_{1};

  // ── Widget registry ───────────────────────────────────────────────────────
  std::unordered_map<std::string, lv_obj_t*> widgets_;

  // ── Startup layout path ───────────────────────────────────────────────────
  std::string startup_layout_;

  // ── Active Page ───────────────────────────────────────────────────────────
  std::string active_page_id_;

  // ── Local sensors ─────────────────────────────────────────────────────────
#ifdef USE_SENSOR
  std::unordered_map<std::string, sensor::Sensor*> local_sensors_;
#endif

  // ── Callback managers (fire automation triggers) ──────────────────────────
  CallbackManager<void()> on_sleep_callbacks_;
  CallbackManager<void()> on_wake_callbacks_;
  CallbackManager<void()> on_swipe_left_callbacks_;
  CallbackManager<void()> on_swipe_right_callbacks_;
  CallbackManager<void()> on_swipe_up_callbacks_;
  CallbackManager<void()> on_swipe_down_callbacks_;
  CallbackManager<void(std::string)> on_page_show_callbacks_;
  CallbackManager<void(std::string)> on_page_hide_callbacks_;

  // ── Internal helpers ──────────────────────────────────────────────────────
  void enter_sleep();
  // wake_up() moved to public — called from static shield callback
  void set_backlight(float level);
  void spawn_shield();
  void destroy_shield();

  // JSONL engine
  void       parse_jsonl_line(const std::string &line);
  lv_obj_t  *create_widget(const std::string &type, lv_obj_t *parent);
  void       apply_properties(lv_obj_t *obj, JsonObject props);
  void       register_widget_events(lv_obj_t *obj, const std::string &id);
  void       load_layout_from_file(const std::string &path);

  // Color helper
  static lv_color_t parse_hex_color(const char *hex);

  // ── API service methods (registered in setup()) ───────────────────────────
  void service_run_jsonl(std::string line);
  void service_clear();
  void service_load_layout(std::string filename);
  void service_save_layout_line(std::string line, bool append);
  void service_bind_sensor(std::string widget_id, std::string sensor_id, std::string format);
  void service_load_page(std::string page_id);
};

// ---------------------------------------------------------------------------
// SleepTrigger / WakeTrigger — inline constructor implementations
// ---------------------------------------------------------------------------
inline SleepTrigger::SleepTrigger(SentioComponent *parent) {
  parent->add_on_sleep_callback([this]() { this->trigger(); });
}

inline WakeTrigger::WakeTrigger(SentioComponent *parent) {
  parent->add_on_wake_callback([this]() { this->trigger(); });
}

inline SwipeLeftTrigger::SwipeLeftTrigger(SentioComponent *parent) {
  parent->add_on_swipe_left_callback([this]() { this->trigger(); });
}

inline SwipeRightTrigger::SwipeRightTrigger(SentioComponent *parent) {
  parent->add_on_swipe_right_callback([this]() { this->trigger(); });
}

inline SwipeUpTrigger::SwipeUpTrigger(SentioComponent *parent) {
  parent->add_on_swipe_up_callback([this]() { this->trigger(); });
}

inline SwipeDownTrigger::SwipeDownTrigger(SentioComponent *parent) {
  parent->add_on_swipe_down_callback([this]() { this->trigger(); });
}

inline PageShowTrigger::PageShowTrigger(SentioComponent *parent) {
  parent->add_on_page_show_callback([this](std::string page_id) { this->trigger(page_id); });
}

inline PageHideTrigger::PageHideTrigger(SentioComponent *parent) {
  parent->add_on_page_hide_callback([this](std::string page_id) { this->trigger(page_id); });
}

}  // namespace sentio
}  // namespace esphome
