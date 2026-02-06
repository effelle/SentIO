#pragma once
#include "esphome.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace sentio {

// The Brain: State Machine
enum TouchState {
  STATE_IDLE,     // Waiting
  STATE_START,    // Touched, calculating intent
  STATE_DRAGGING, // Moving > threshold (Swipe)
  STATE_RELEASED  // Let go
};

class SmartTouchComponent : public touchscreen::Touchscreen, public Component {
public:
  // --- Setup & Config ---
  void set_source_driver(touchscreen::Touchscreen *source) {
    source_driver_ = source;
  }
  void set_resolution(int w, int h) {
    display_width_ = w;
    display_height_ = h;
  }
  void set_sleep_timeout(uint32_t t) { sleep_timeout_ms_ = t; }
  void set_suppress_wake_click(bool b) { suppress_wake_click_ = b; }
  void set_calibration(bool swap, bool inv_x, bool inv_y) {
    swap_xy_ = swap;
    invert_x_ = inv_x;
    invert_y_ = inv_y;
  }
  void set_debounce_threshold(uint32_t ms) { debounce_ms_ = ms; }
  void set_debug_raw(bool b) { debug_raw_ = b; }

  // --- Triggers (Automation hooks) ---
  Trigger<> *get_trigger(const std::string &conf);
  void set_on_swipe_left(Trigger<> *t) { on_swipe_left_ = t; }
  void set_on_swipe_right(Trigger<> *t) { on_swipe_right_ = t; }
  void set_on_tap(Trigger<> *t) { on_tap_ = t; }
  void set_on_wake(Trigger<> *t) { on_wake_ = t; }
  void set_on_sleep(Trigger<> *t) { on_sleep_ = t; }

  // --- Lifecycle ---
  void setup() override;
  void loop() override;

protected:
  // Internal Logic
  touchscreen::Touchscreen *source_driver_{nullptr};

  // Config Variables
  int display_width_, display_height_;
  uint32_t sleep_timeout_ms_;
  bool suppress_wake_click_, swap_xy_, invert_x_, invert_y_, debug_raw_;
  uint32_t debounce_ms_;

  // Runtime State
  uint32_t last_activity_time_{0};
  bool is_sleeping_{false};
  bool ignore_next_release_{false}; // The Trap Flag

  // Gesture State
  TouchState state_{STATE_IDLE};
  uint32_t gesture_start_time_{0};
  int16_t start_x_{0}, start_y_{0};

  // Triggers
  Trigger<> *on_swipe_left_{nullptr};
  Trigger<> *on_swipe_right_{nullptr};
  Trigger<> *on_tap_{nullptr};
  Trigger<> *on_wake_{nullptr};
  Trigger<> *on_sleep_{nullptr};

  // Helpers
  touchscreen::TouchPoint apply_calibration(touchscreen::TouchPoint p);
  void process_gestures(touchscreen::TouchPoint p);
  void handle_release();
};

} // namespace sentio
} // namespace esphome
