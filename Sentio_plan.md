# Project: ESPHome Smart Touch Input Subsystem
**Goal:** Create a "Universal" Input Subsystem for ESPHome that fixes the fragmentation and poor user experience of cheap embedded displays (CYD, Sunton, etc.). The component will be called `sentio` and MUST work with both ESP-IDF and Arduino Framework.


## 1. The Core Philosophy: The Proxy Pattern
To avoid writing 50 different hardware drivers, we will use a **Man-in-the-Middle (Proxy)** architecture.

*   **The Source:** An existing ESPHome touchscreen platform (GT911, CST816, etc.) configured as `internal`.
*   **The Component (`sentio`):** Reads the source, sanitizes the data, applies logic, and publishes clean events.
*   **The Consumer:** LVGL, Home Assistant, or Lambda scripts listen to `sentio`, not the hardware.

---

## 2. The Problems ("The Plagues") & Solutions

| The Plague | The Symptom | The `sentio` Solution |
| :--- | :--- | :--- |
| **The "Wake-up Click"** | Tapping a dark screen to wake it up accidentally turns on a light/switch located at that coordinate. | **Suppression Logic:** If state is `SLEEPING`, the first touch event wakes the system but is **swallowed** (not passed to the application). |
| **Sleep of Death** | Touch chips (CST816) sleep deeply and cannot be woken via I2C, requiring a hardware `RST` toggle. If `RST` isn't wired, the screen is bricked. | **Safety Check:** If user does not define a `reset_pin` in config, force **"Soft Sleep"** (stop polling data, but keep chip powered) instead of sending I2C Sleep commands. |
| **Ghost Touches** | Noisy WiFi power supplies cause random single-frame clicks or jitter. | **Min-Frame Filter:** Ignore any touch event that lasts < `N` ms (e.g., 2 frames). |
| **Coordinate Hell** | Screen rotation doesn't match touch coordinates. Users struggle with `swap_xy` and `mirror`. | **Pipeline Calibration:** Apply mathematical transforms (Swap -> Invert -> Offset) *before* the gesture engine sees the data. |
| **Swipe is a Click** | Scrolling a list in LVGL accidentally triggers `on_press` for buttons. | **Gesture Debounce:** A State Machine that separates "Taps" from "Drags." |

---

## 3. Development Roadmap

### Phase 1: The "Passthrough" POC
*   **Goal:** Get the component to compile and successfully forward touches from `gt911` to `lvgl`.
*   **Verify:** Using the "Red Dot" visualization tip.
    *   *Tip:* In your display lambda: `it.filled_circle(touch.x, touch.y, 5, id(red));`
    Example:
    ```yaml
        display:
      lambda: |-
        // draw UI...
        
        // Debug Dot
        for (auto touch : id(main_touchscreen).touches) {
          it.filled_circle(touch.x, touch.y, 5, id(my_red_color));
        }
    ```
*   **Test:** Ensure `internal: true` works and you don't get duplicate inputs.

### Phase 2: The "Sleep Doctor"
*   **Goal:** Implement the Timeout and Wake-up logic.
*   **Critical Test:** Let screen sleep. Tap a button.
    *   *Success:* Screen wakes, button DOES NOT toggle.
    *   *Fail:* Screen wakes, button toggles.
*   **Safety Implementation:** Add the check for `reset_pin`. If null, implement "Soft Sleep" (just stop updating `last_activity_time` logic but don't send I2C commands).

### Phase 3: The Calibrator
*   **Goal:** Implement `swap_xy`, `invert_x`, `invert_y`.
*   **Debug Mode:** Implement `debug_raw_touch: true` which dumps `RAW(x,y) -> CALIB(x,y)` to serial logs. This replaces the need for a runtime wizard.

### Phase 4: The Gesture Engine (Final Polish)
*   **Goal:** Add `on_swipe` and `on_tap`.
*   **Logic:** Implement the State Machine (`IDLE` -> `START` -> `DRAG`).
    *   If movement > `swipe_threshold`, fire Swipe event.
    *   If movement < `swipe_threshold` AND released, fire Tap event.

---

## 4. Pro-Tips & Pitfalls

1.  **I2C Address Scanning:**
    *   Don't trust the ESPHome boot scanner.
    *   If implementing a "Universal Hardware Driver" later, always toggle `RST`/`INT` pins manually *before* `Wire.begin()` to latch the correct address.

2.  **Visual Debugging:**
    *   Drawing the "Red Dot" on the screen is 10x faster than reading Serial Logs to verify coordinates.

3.  **The "Poll" Order:**
    *   Your `sentio` component is technically a consumer. Ensure it processes data *after* the hardware driver updates. Usually, placing the YAML config for `sentio` *below* the hardware driver is enough, but keep this in mind if you see "lag".

4.  **Multi-Touch:**
    *   Stick to single-point touch for the MVP. Most cheap screens (CST816, XPT2046) only support one point properly. Handling multi-touch arrays complicates the "Ghost Touch" filters significantly.


## 5. Workflow

1. Create the folder .agent/workflows if not exists. Inside put the file sentio-dev.md that you will use to feed your knowledge base using the relevant information you have found in this file.
2. All the file for the component you create will be saved on SentIO folder.

### What follow is the **Master Implementation Blueprint**. It contains the specific logic, algorithms, and code structures required to build the `Sentio` external component.

---

# 1. Component Architecture & Files
You will create a generic External Component.
**Directory Structure:**
```
components/
  Sentio/
    __init__.py       # Python Configuration Logic
    Sentio.h     # C++ Header (Class Definition)
    Sentio.cpp   # C++ Implementation (The Logic)
```

---

# 2. Python Configuration (`__init__.py`)
This file defines how the YAML is validated and mapped to C++.

**Key Details Missing Previously:**
*   We must capture the **Resolution** (`display_width/height`) to perform `invert_x/y` math correctly.
*   We define **Triggers** (`on_swipe`, `on_sleep`) to allow users to attach automations.

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import touchscreen, binary_sensor
from esphome.const import CONF_ID, CONF_SOURCE, CONF_OUTPUT_ID

# Namespace
Sentio_ns = cg.esphome_ns.namespace('Sentio')
SmartTouchComponent = Sentio_ns.class_('SmartTouchComponent', touchscreen.Touchscreen, cg.Component)

# Configuration Constants
CONF_DISPLAY_WIDTH = "display_width"
CONF_DISPLAY_HEIGHT = "display_height"
CONF_SLEEP_TIMEOUT = "sleep_timeout"
CONF_SUPPRESS_WAKE_CLICK = "suppress_wake_click"
CONF_SWAP_XY = "swap_xy"
CONF_INVERT_X = "invert_x"
CONF_INVERT_Y = "invert_y"
CONF_DEBOUNCE_THRESHOLD = "debounce_threshold"
CONF_DEBUG_RAW = "debug_raw_touch"

# Triggers
CONF_ON_SWIPE_LEFT = "on_swipe_left"
CONF_ON_SWIPE_RIGHT = "on_swipe_right"
CONF_ON_TAP = "on_tap"
CONF_ON_WAKE = "on_wake"
CONF_ON_SLEEP = "on_sleep"

CONFIG_SCHEMA = touchscreen.TOUCHSCREEN_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(SmartTouchComponent),
    cv.Required(CONF_SOURCE): cv.use_id(touchscreen.Touchscreen),
    
    # Resolution (Required for Inversion Math)
    cv.Required(CONF_DISPLAY_WIDTH): cv.int_,
    cv.Required(CONF_DISPLAY_HEIGHT): cv.int_,

    # Power Management
    cv.Optional(CONF_SLEEP_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_SUPPRESS_WAKE_CLICK, default=True): cv.boolean,

    # Calibration
    cv.Optional(CONF_SWAP_XY, default=False): cv.boolean,
    cv.Optional(CONF_INVERT_X, default=False): cv.boolean,
    cv.Optional(CONF_INVERT_Y, default=False): cv.boolean,
    cv.Optional(CONF_DEBOUNCE_THRESHOLD, default="20ms"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_DEBUG_RAW, default=False): cv.boolean,

    # Gestures
    cv.Optional(CONF_ON_SWIPE_LEFT): cv.automation_schema,
    cv.Optional(CONF_ON_SWIPE_RIGHT): cv.automation_schema,
    cv.Optional(CONF_ON_TAP): cv.automation_schema,
    cv.Optional(CONF_ON_WAKE): cv.automation_schema,
    cv.Optional(CONF_ON_SLEEP): cv.automation_schema,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await touchscreen.register_touchscreen(var, config)

    # Link the Source Driver
    source = await cg.get_variable(config[CONF_SOURCE])
    cg.add(var.set_source_driver(source))

    # Set Configuration
    cg.add(var.set_resolution(config[CONF_DISPLAY_WIDTH], config[CONF_DISPLAY_HEIGHT]))
    cg.add(var.set_sleep_timeout(config[CONF_SLEEP_TIMEOUT]))
    cg.add(var.set_suppress_wake_click(config[CONF_SUPPRESS_WAKE_CLICK]))
    cg.add(var.set_calibration(config[CONF_SWAP_XY], config[CONF_INVERT_X], config[CONF_INVERT_Y]))
    cg.add(var.set_debounce_threshold(config[CONF_DEBOUNCE_THRESHOLD]))
    cg.add(var.set_debug_raw(config[CONF_DEBUG_RAW]))

    # Register Triggers
    for conf, trigger_fn in [
        (CONF_ON_SWIPE_LEFT, var.set_on_swipe_left),
        (CONF_ON_SWIPE_RIGHT, var.set_on_swipe_right),
        (CONF_ON_TAP, var.set_on_tap),
        (CONF_ON_WAKE, var.set_on_wake),
        (CONF_ON_SLEEP, var.set_on_sleep),
    ]:
        if conf in config:
            await cv.automation.build_automation(var.get_trigger(conf), [], config[conf])
```

---

# 3. The Header Logic (`Sentio.h`)
This file defines the **State Machine** and the **Trigger** objects.

**Key Details Added:**
*   `AutomationTrigger`: Standard way to fire YAML actions from C++.
*   `TouchState`: The enum for the gesture engine.

```cpp
#pragma once
#include "esphome.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace Sentio {

// The Brain: State Machine
enum TouchState {
  STATE_IDLE,       // Waiting
  STATE_START,      // Touched, calculating intent
  STATE_DRAGGING,   // Moving > threshold (Swipe)
  STATE_RELEASED    // Let go
};

class SmartTouchComponent : public touchscreen::Touchscreen, public Component {
 public:
  // --- Setup & Config ---
  void set_source_driver(touchscreen::Touchscreen *source) { source_driver_ = source; }
  void set_resolution(int w, int h) { display_width_ = w; display_height_ = h; }
  void set_sleep_timeout(uint32_t t) { sleep_timeout_ms_ = t; }
  void set_suppress_wake_click(bool b) { suppress_wake_click_ = b; }
  void set_calibration(bool swap, bool inv_x, bool inv_y) { 
    swap_xy_ = swap; invert_x_ = inv_x; invert_y_ = inv_y; 
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

} // namespace Sentio
} // namespace esphome
```

---

# 4. The Implementation (`Sentio.cpp`)
Here is where the magic happens.

**Key Algorithms Explained:**
1.  **Calibration:** Note the order. `swap` first, then `invert`. This is standard.
2.  **Debounce:** We track `gesture_start_time_`. If the touch is released too fast (< `debounce_ms_`), we treat it as noise (Ghost Touch) and ignore it.
3.  **Swipe vs Tap:** We check `abs(current_x - start_x)`. If > 30px (hardcoded reasonable threshold), it becomes a SWIPE. If you release before moving that much, it's a TAP.

```cpp
#include "Sentio.h"

namespace esphome {
namespace Sentio {

static const int SWIPE_THRESHOLD = 30; // Pixels to trigger a swipe
static const int MAX_TAP_TIME = 400;   // Max ms for a tap (otherwise it's a hold)

void SmartTouchComponent::setup() {
  this->last_activity_time_ = millis();
}

void SmartTouchComponent::loop() {
  if (this->source_driver_ == nullptr) return;

  // 1. SLEEP CHECK
  if (millis() - this->last_activity_time_ > this->sleep_timeout_ms_) {
    if (!this->is_sleeping_) {
      this->is_sleeping_ = true;
      ESP_LOGI("Sentio", "Entering Sleep Mode");
      if (this->on_sleep_) this->on_sleep_->trigger();
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
    if (this->on_wake_) this->on_wake_->trigger();

    if (this->suppress_wake_click_) {
      this->ignore_next_release_ = true; // Set trap
      return; // Swallow this frame
    }
  }

  // Reset timer
  this->last_activity_time_ = millis();

  // If trap is set (wake-up click), ignore everything until release
  if (this->ignore_next_release_) return;

  // 6. CALIBRATE
  auto p = this->apply_calibration(raw_p);

  // 7. GESTURE & DEBOUNCE ENGINE
  this->process_gestures(p);

  // 8. OUTPUT TO CONSUMERS (LVGL)
  // Only update LVGL if we passed the debounce check (handled in process_gestures)
  // For the MVP, we just pass it through, but ideally, we wait `debounce_ms`
  this->add_raw_touch_position_(p.id, p.x, p.y, p.pressure);
}

touchscreen::TouchPoint SmartTouchComponent::apply_calibration(touchscreen::TouchPoint p) {
  int x = p.x;
  int y = p.y;

  // 1. Swap
  if (this->swap_xy_) std::swap(x, y);

  // 2. Invert (Requires display resolution)
  // Note: If swapped, x is now relative to the *height* dimension
  int width = this->swap_xy_ ? this->display_height_ : this->display_width_;
  int height = this->swap_xy_ ? this->display_width_ : this->display_height_;

  if (this->invert_x_) x = width - x;
  if (this->invert_y_) y = height - y;
  
  // Clamp to 0
  if (x < 0) x = 0;
  if (y < 0) y = 0;

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
           if(this->on_swipe_right_) this->on_swipe_right_->trigger();
        } else {
           if(this->on_swipe_left_) this->on_swipe_left_->trigger();
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
       if (this->on_tap_) this->on_tap_->trigger();
    }
  }
}

// Boilerplate to register triggers
Trigger<> *SmartTouchComponent::get_trigger(const std::string &conf) {
  if (conf == "on_swipe_left") return this->on_swipe_left_;
  // ... (implement others)
  return nullptr;
}

} // namespace Sentio
} // namespace esphome
```

---

# 5. Integration: How to use it
This is the copy-paste YAML for your user documentation.

```yaml
# 1. Define the Raw Hardware (Internal)
touchscreen:
  - platform: gt911
    id: my_hardware_touch
    internal: true # Hide from HA
    i2c_id: bus_a
    interrupt_pin: GPIO4
    reset_pin: GPIO16 

# 2. Define the Smart Proxy
  - platform: Sentio
    id: my_Sentio
    source: my_hardware_touch
    
    # Matching your display resolution
    display_width: 320
    display_height: 240
    
    # Power Settings
    sleep_timeout: 15s
    suppress_wake_click: true 
    
    # Calibration
    swap_xy: true
    invert_x: true
    
    # Noise Filter
    debounce_threshold: 20ms
    
    # Gestures
    on_swipe_left:
      - logger.log: "Previous Page"
    on_swipe_right:
      - logger.log: "Next Page"
    on_tap:
      - logger.log: "Tap Detected"
      
    # Advanced Power Control
    on_sleep:
      - light.turn_off: backlight
    on_wake:
      - light.turn_on: backlight

# 3. Connect Display to Smart Proxy
display:
  - platform: ili9341
    # ...
    touchscreen_id: my_Sentio # <--- Connects to proxy
```

### Critical Implementation Note
There is one tricky part in `__init__.py` regarding triggers. Because the component generates triggers dynamically based on the YAML, you need to ensure the C++ `get_trigger` method handles the string mapping.

In the `to_code` python function, I used `var.set_on_swipe_left` setters. This is cleaner. In C++, ensure the member variables (`on_swipe_left_`) are initialized to `nullptr` and checked before calling `->trigger()`.

This blueprint provides the complete loop logic, the math for calibration, and the specific architecture to hide the dirty hardware details from the clean UI layer.

### Github repository

1. SentIO will be uploaded to https://github.com/effelle/SentIO you will init the repository and upload all the relevant files there:
```bash
git init
git add .
git commit -m "Initial Release of SentIO v1.0.0"
```

Then
```bash
git remote add origin https://github.com/effelle/SentIO.git
git branch -M main
git push -u origin main
```

2. Create the library/manifest files (e.g. library.json) to reflect the name 'SentIO' and the new GitHub URL.
3. You will add to .gitignore the local folder Esphome. It contain the developer wiki (developers.esphome.io-main) and the latest esphome code (esphome-dev folder). 