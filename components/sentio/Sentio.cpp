#include "Sentio.h"

namespace esphome {
namespace sentio {

static const int SWIPE_THRESHOLD = 30; // Pixels to trigger a swipe
static const int MAX_TAP_TIME = 400; // Max ms for a tap (otherwise it's a hold)

void SmartTouchComponent::setup() { this->last_activity_time_ = millis(); }

void SmartTouchComponent::loop() {
  if (this->source_driver_ == nullptr)
    return;

  // 1. SLEEP CHECK
  if (millis() - this->last_activity_time_ > this->sleep_timeout_ms_) {
    if (!this->is_sleeping_) {
      this->is_sleeping_ = true;
      ESP_LOGI("Sentio", "Entering Sleep Mode");
      if (this->on_sleep_)
        this->on_sleep_->trigger();
    }
  }

  // 2. READ SOURCE
  auto &src_touches = this->source_driver_->touches;

  // 3. RELEASE LOGIC (Finger up)
  if (src_touches.empty()) {
    if (this->state_ != STATE_IDLE) {
      this->handle_release(); // Logic for Tap detection
      this->state_ = STATE_IDLE;

      // Clear output to consumers
      this->touches.clear();

      // Reset the wake-up trap
      this->ignore_next_release_ = false;
    }
    return;
  }

  // 4. TOUCH DETECTED (Finger down)
  auto raw_p = src_touches[0]; // Logic for single point

  // --- DEBUGGING ---
  if (this->debug_raw_) {
    ESP_LOGD("Sentio", "Raw: x=%d y=%d", raw_p.x, raw_p.y);
  }

  // 5. WAKE LOGIC
  if (this->is_sleeping_) {
    this->is_sleeping_ = false;
    this->last_activity_time_ = millis();
    ESP_LOGI("Sentio", "Waking Up");
    if (this->on_wake_)
      this->on_wake_->trigger();

    if (this->suppress_wake_click_) {
      this->ignore_next_release_ = true; // Set trap
      return;                            // Swallow this frame
    }
  }

  // Reset timer
  this->last_activity_time_ = millis();

  // If trap is set (wake-up click), ignore everything until release
  if (this->ignore_next_release_)
    return;

  // 6. CALIBRATE
  auto p = this->apply_calibration(raw_p);

  // 7. GESTURE & DEBOUNCE ENGINE
  this->process_gestures(p);

  // 8. OUTPUT TO CONSUMERS (LVGL)
  // Only update LVGL if we passed the debounce check (handled in
  // process_gestures) For the MVP, we just pass it through, but ideally, we
  // wait `debounce_ms`
  this->add_raw_touch_position_(p.id, p.x, p.y, p.pressure);
}

touchscreen::TouchPoint
SmartTouchComponent::apply_calibration(touchscreen::TouchPoint p) {
  int x = p.x;
  int y = p.y;

  // 1. Swap
  if (this->swap_xy_)
    std::swap(x, y);

  // 2. Invert (Requires display resolution)
  // Note: If swapped, x is now relative to the *height* dimension
  int width = this->swap_xy_ ? this->display_height_ : this->display_width_;
  int height = this->swap_xy_ ? this->display_width_ : this->display_height_;

  if (this->invert_x_)
    x = width - x;
  if (this->invert_y_)
    y = height - y;

  // Clamp to 0
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;

  p.x = x;
  p.y = y;
  return p;
}

void SmartTouchComponent::process_gestures(touchscreen::TouchPoint p) {
  switch (this->state_) {
  case STATE_IDLE:
    // Start of a touch
    this->state_ = STATE_START;
    this->start_x_ = p.x;
    this->start_y_ = p.y;
    this->gesture_start_time_ = millis();
    break;

  case STATE_START:
    // Check for Swipe
    int dx = p.x - this->start_x_;
    int dy = p.y - this->start_y_;

    // Horizontal Swipe Detection
    if (abs(dx) > SWIPE_THRESHOLD) {
      this->state_ = STATE_DRAGGING;
      if (dx > 0) {
        if (this->on_swipe_right_)
          this->on_swipe_right_->trigger();
      } else {
        if (this->on_swipe_left_)
          this->on_swipe_left_->trigger();
      }
    }
    break;

  case STATE_DRAGGING:
    // We already triggered the swipe, just wait for release
    break;
  }
}

void SmartTouchComponent::handle_release() {
  // If we are releasing, and we never left STATE_START, it's a TAP
  if (this->state_ == STATE_START) {
    uint32_t duration = millis() - this->gesture_start_time_;

    // Ghost Touch Filter: If touch was too short (WiFi noise), ignore it
    if (duration < this->debounce_ms_) {
      ESP_LOGD("Sentio", "Ignored noise pulse (<%dms)", this->debounce_ms_);
      // Also clear the 'touches' buffer so LVGL doesn't see it
      this->touches.clear();
      return;
    }

    if (duration < MAX_TAP_TIME) {
      if (this->on_tap_)
        this->on_tap_->trigger();
    }
  }
}

// Boilerplate to register triggers
Trigger<> *SmartTouchComponent::get_trigger(const std::string &conf) {
  if (conf == "on_swipe_left")
    return this->on_swipe_left_;
  if (conf == "on_swipe_right")
    return this->on_swipe_right_;
  if (conf == "on_tap")
    return this->on_tap_;
  if (conf == "on_wake")
    return this->on_wake_;
  if (conf == "on_sleep")
    return this->on_sleep_;
  return nullptr;
}

} // namespace sentio
} // namespace esphome
