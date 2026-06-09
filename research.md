## Research May 2026 - SentIO

### 1. Touch & Input Management (High Priority)
*   **The "Swipe vs. Click" Collision:** This is currently one of the biggest pain points. Users love the new swipe gestures to change pages, but if they swipe across a button, the button registers a "short click" and triggers an action. **What you can do:** Implement a touch threshold or input delay in the input driver. If the touch registers movement (a drag/swipe) beyond a few pixels, automatically cancel the `on_short_click` event for underlying widgets.
*   **LVGL Group Support for Encoders:** Currently, ESPHome lacks a straightforward way to create and assign different LVGL groups to specific widgets dynamically (unless tied directly to a physical keypad/encoder). If you are working on input overlays, allowing users to map rotary encoders to specific widget groups seamlessly would be highly praised.

### 2. Display & Power Management
Since you already added "tap to wake," here is what users want next in that specific domain:
*   **Smooth Dimming & Fade-outs:** Instead of the screen just cutting to black when the idle timeout hits, users want a configurable hardware fade-out (PWM dimming) natively handled by the LVGL driver. 
*   **Animation & Refresh Pausing:** When the screen turns off (or dims completely), the ESP32 CPU is often still hammering away at rendering LVGL animations in the background or updating sensor labels. Adding a hook to pause the `lv_task_handler` or lower the refresh rate when the display sleeps would save massive amounts of CPU cycles and prevent overheating.
*   **Native Burn-In Protection:** OLED and some LCD users have to hack together "pixel shifting" or screensavers via messy YAML. A built-in driver toggle for `anti_burn_in: true` that shifts the entire LVGL viewport by 1-2 pixels periodically would be a massive quality-of-life feature.

### 3. Missing Widgets (C++ to YAML Bindings)
Several native LVGL widgets have not been ported/exposed to ESPHome YAML yet, and users frequently request them:
*   **Charts (`lv_chart`):** The number one requested widget. Users want to display home temperature history or power usage directly on their wall panels, but cannot do so without dropping out of ESPHome and writing raw C++ in PlatformIO.
*   **Color Picker (`lv_cpicker`):** Highly requested for smart home dashboards to control RGB lights natively.
*   **Lottie Animations (`rlottie`):** Users want modern, fluid loading icons and weather animations. Wrapping the LVGL Lottie player into ESPHome YAML is a highly tracked feature request.

### 4. Page Routing & Layer Logic
*   **`on_show` and `on_hide` Triggers:** Users want to trigger hardware actions (like turning on a local LED or requesting an aggressive sensor update) *only* when a specific LVGL page or widget becomes visible, but these triggers are missing.
*   **`is_displaying` Condition:** Users want to put a clock or Wi-Fi icon on the `top_layer` so it persists across pages, but they want the ability to hide it conditionally (e.g., "Show this top-layer widget EXCEPT when on the screensaver page"). There is currently no `is_displaying.page` YAML condition.

### 5. Data Binding & Memory Quirks
*   **Flicker-Free Background Switching:** Users dynamically switching backgrounds based on states (e.g., changing the background to rain/sun depending on HA weather) report display flickering and `draw_bg_img: Couldn't read the background image` memory timeout errors. If your driver overlay can manage image caching or double-buffering more gracefully, it would solve a major UI grievance.
*   **The Lambda Formatting Nightmare:** A massive complaint on Reddit is how difficult it is to get a standard Home Assistant sensor (like a battery percentage or temperature with a decimal) to display on an LVGL label. Users are forced to write C++ `sprintf` lambdas. While this is more of a core ESPHome string-templating issue, any helper functions you can expose to easily bind a sensor ID to an LVGL label's text without C++ would make you a hero.