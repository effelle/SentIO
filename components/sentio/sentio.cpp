#include "sentio.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"

// LittleFS — Arduino framework only (IDF path is a Phase 4 enhancement)
#ifdef USE_ARDUINO
#  include "FS.h"
#  include "LittleFS.h"
#endif



// Pull in the full LVGL API so all widget create/class/get symbols are
// available. sentio.h already includes lvgl.h via the sentio header, but
// ESPHome may not guarantee that all widget headers are transitively
// included — this explicit include ensures they are.
#include "lvgl.h"

namespace esphome {
namespace sentio {

// Static singleton — set in setup() so lambdas in on_boot can reach us
// without relying on ESPHome's generated ID variable.
SentioComponent *SentioComponent::instance = nullptr;

static const char *const TAG = "sentio";

// Swipe threshold in pixels before a drag is classified as a swipe
static constexpr int16_t SWIPE_THRESHOLD = 30;

// ---------------------------------------------------------------------------
// Static LVGL callbacks (must be free functions — no captures allowed)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LVGL filesystem drivers — shared POSIX callbacks + per-mount open functions
//
// L: → /littlefs  (internal flash, always present)
// S: → /sdcard    (SD card, only when USE_SENTIO_SD and card is mounted)
// ---------------------------------------------------------------------------

static lv_fs_drv_t sentio_fs_drv;

#ifdef USE_SENTIO_SD
lv_fs_drv_t SentioComponent::sentio_sd_fs_drv_;
#endif

// ── Shared POSIX callbacks (identical for both drivers) ───────────────────

static lv_fs_res_t sentio_fs_close_cb(lv_fs_drv_t *, void *fp) {
  if (fp != nullptr) fclose(static_cast<FILE *>(fp));
  return LV_FS_RES_OK;
}

static lv_fs_res_t sentio_fs_read_cb(lv_fs_drv_t *, void *fp, void *buf, uint32_t btr, uint32_t *br) {
  if (fp == nullptr) return LV_FS_RES_HW_ERR;
  size_t n = fread(buf, 1, btr, static_cast<FILE *>(fp));
  if (br != nullptr) *br = static_cast<uint32_t>(n);
  return LV_FS_RES_OK;
}

static lv_fs_res_t sentio_fs_write_cb(lv_fs_drv_t *, void *fp, const void *buf, uint32_t btw, uint32_t *bw) {
  if (fp == nullptr) return LV_FS_RES_HW_ERR;
  size_t n = fwrite(buf, 1, btw, static_cast<FILE *>(fp));
  if (bw != nullptr) *bw = static_cast<uint32_t>(n);
  return LV_FS_RES_OK;
}

static lv_fs_res_t sentio_fs_seek_cb(lv_fs_drv_t *, void *fp, uint32_t pos, lv_fs_whence_t whence) {
  if (fp == nullptr) return LV_FS_RES_HW_ERR;
  int w = SEEK_SET;
  if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
  else if (whence == LV_FS_SEEK_END) w = SEEK_END;
  return fseek(static_cast<FILE *>(fp), pos, w) == 0 ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_res_t sentio_fs_tell_cb(lv_fs_drv_t *, void *fp, uint32_t *pos_p) {
  if (fp == nullptr) return LV_FS_RES_HW_ERR;
  long pos = ftell(static_cast<FILE *>(fp));
  if (pos < 0) return LV_FS_RES_HW_ERR;
  if (pos_p != nullptr) *pos_p = static_cast<uint32_t>(pos);
  return LV_FS_RES_OK;
}

// ── Per-mount open functions ───────────────────────────────────────────────

// L: — always routes to /littlefs
static void *sentio_lfs_open_cb(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode) {
  const char *ms = (mode == LV_FS_MODE_WR) ? "w" : "r";
  std::string full = (path[0] == '/') ? "/littlefs" : "/littlefs/";
  full += path;
  return static_cast<void *>(fopen(full.c_str(), ms));
}

#ifdef USE_SENTIO_SD
// S: — always routes to /sdcard
static void *sentio_sd_open_cb(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode) {
  const char *ms = (mode == LV_FS_MODE_WR) ? "w" : "r";
  std::string full = (path[0] == '/') ? "/sdcard" : "/sdcard/";
  full += path;
  return static_cast<void *>(fopen(full.c_str(), ms));
}
#endif

// ── Helper used by all POSIX file-path callers in this component ──────────
//
// Priority:
//   1. Explicit /sdcard/…  → SD (even if SD absent — fopen will fail normally)
//   2. Explicit /littlefs/… → LittleFS
//   3. Bare path            → SD if mounted, else LittleFS
static std::string resolve_path(const std::string &path, bool sd_mounted) {
  if (path.rfind("/sdcard",   0) == 0) return path;
  if (path.rfind("/littlefs", 0) == 0) return path;
  std::string bare = (path.empty() || path[0] != '/') ? "/" + path : path;
  return sd_mounted ? "/sdcard" + bare : "/littlefs" + bare;
}

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
  instance = this;

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
  lv_obj_clear_flag(root_container_, LV_OBJ_FLAG_CLICKABLE);

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
  register_service(&SentioComponent::service_set_bg_image,
                   "sentio_set_bg_image", {"widget_id", "path"});

  // Register LVGL LittleFS driver (L:)
  lv_fs_drv_init(&sentio_fs_drv);
  sentio_fs_drv.letter   = 'L';
  sentio_fs_drv.open_cb  = sentio_lfs_open_cb;
  sentio_fs_drv.close_cb = sentio_fs_close_cb;
  sentio_fs_drv.read_cb  = sentio_fs_read_cb;
  sentio_fs_drv.write_cb = sentio_fs_write_cb;
  sentio_fs_drv.seek_cb  = sentio_fs_seek_cb;
  sentio_fs_drv.tell_cb  = sentio_fs_tell_cb;
  lv_fs_drv_register(&sentio_fs_drv);

#ifdef USE_SENTIO_SD
  if (sd_manager_.mount(sd_cfg_)) {
    lv_fs_drv_init(&sentio_sd_fs_drv_);
    sentio_sd_fs_drv_.letter   = 'S';
    sentio_sd_fs_drv_.open_cb  = sentio_sd_open_cb;
    sentio_sd_fs_drv_.close_cb = sentio_fs_close_cb;
    sentio_sd_fs_drv_.read_cb  = sentio_fs_read_cb;
    sentio_sd_fs_drv_.write_cb = sentio_fs_write_cb;
    sentio_sd_fs_drv_.seek_cb  = sentio_fs_seek_cb;
    sentio_sd_fs_drv_.tell_cb  = sentio_fs_tell_cb;
    lv_fs_drv_register(&sentio_sd_fs_drv_);
    ESP_LOGI(TAG, "LVGL SD driver registered (S:)");
  } else {
    ESP_LOGW(TAG, "SD card unavailable — S: driver not registered");
  }
#endif

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

  // ── Hook pointer input devices ───────────────────────────────────────────
  this->hook_input_devices();

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
#ifdef USE_SENTIO_SD
  const char *sd_mode_str = "?";
  switch (sd_cfg_.mode) {
    case SdMode::SDMMC_4BIT: sd_mode_str = "SDMMC 4-bit"; break;
    case SdMode::SDMMC_1BIT: sd_mode_str = "SDMMC 1-bit"; break;
    case SdMode::SPI:        sd_mode_str = "SPI";         break;
  }
  ESP_LOGCONFIG(TAG, "  SD card       : %s, %s",
                sd_mode_str, sd_manager_.is_mounted() ? "mounted" : "FAILED");
  if (sd_manager_.is_mounted()) {
    ESP_LOGCONFIG(TAG, "  SD mount point: %s", sd_manager_.mount_point());
  }
#endif
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
  if (burn_in_timer_ != nullptr) {
    lv_timer_pause(burn_in_timer_);
  }
  // Note: lv_timer_pause_all() pauses ALL timers including LVGL internals.
  // We pause only our own timer here; the user can pause more via on_sleep.

  // Spawn the touch shield so the first wake tap is intercepted safely
  spawn_shield();

  if (soft_sleep_only_ && touch_source_ != nullptr) {
    touch_source_->stop_poller();
  }

  // Fire the on_sleep automation trigger — user YAML decides what happens
  // (deep_sleep.enter, light.turn_off, nothing, etc.)
  on_sleep_callbacks_.call();
}

void SentioComponent::wake_up() {
  if (!is_sleeping_) return;
  is_sleeping_ = false;
  last_activity_ms_ = millis();

  ESP_LOGI(TAG, "Waking up");

  if (soft_sleep_only_ && touch_source_ != nullptr) {
    touch_source_->start_poller();
  }

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
  std::string bare = startup_layout_.empty() ? "/sentio.jsonl" : startup_layout_;
#ifdef USE_SENTIO_SD
  std::string file_path = resolve_path(bare, sd_manager_.is_mounted());
#else
  std::string file_path = resolve_path(bare, false);
#endif
  const char *mode = append ? "a" : "w";
  FILE *f = fopen(file_path.c_str(), mode);
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open layout file '%s' for writing", file_path.c_str());
    return;
  }
  fprintf(f, "%s\n", line.c_str());
  fclose(f);
#endif
}

// ---------------------------------------------------------------------------
// JSONL parser
// ---------------------------------------------------------------------------

void SentioComponent::parse_jsonl_line(const std::string &line) {
  bool ok = json::parse_json(line, [&](JsonObject root) -> bool {
    // Every valid line must have an "id" field
    if (!root["id"].is<JsonVariant>()) {
      ESP_LOGW(TAG, "JSONL line missing 'id' field: %s", line.c_str());
      return false;
    }

    std::string id = root["id"].as<std::string>();

    // Delete command: {"id": "my_widget", "delete": true}
    if (root["delete"].is<bool>() && root["delete"].as<bool>()) {
      auto it = widgets_.find(id);
      if (it != widgets_.end()) {
        lv_obj_del(it->second);
        widgets_.erase(it);
        ESP_LOGD(TAG, "Deleted widget '%s'", id.c_str());
      }
      return true;
    }

    lv_obj_t *obj = nullptr;

    if (root["obj"].is<JsonVariant>()) {
      // Creation mode: widget does not yet exist (or we replace it)
      std::string type = root["obj"].as<std::string>();
      auto it = widgets_.find(id);
      if (it != widgets_.end()) {
        // Replace existing widget
        lv_obj_del(it->second);
        widgets_.erase(it);
      }
      lv_obj_t *parent_obj = root_container_;
      if (root["parent"].is<const char *>()) {
        std::string parent_id = root["parent"].as<std::string>();
        auto pit = widgets_.find(parent_id);
        if (pit != widgets_.end()) {
          parent_obj = pit->second;
        } else {
          ESP_LOGW(TAG, "Parent widget '%s' not found, defaulting to root", parent_id.c_str());
        }
      }
      obj = create_widget(type, parent_obj);
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
      if (root["parent"].is<const char *>()) {
        std::string parent_id = root["parent"].as<std::string>();
        auto pit = widgets_.find(parent_id);
        if (pit != widgets_.end()) {
          lv_obj_set_parent(obj, pit->second);
        } else {
          ESP_LOGW(TAG, "Parent widget '%s' not found, cannot reparent", parent_id.c_str());
        }
      }
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
#if LVGL_VERSION_MAJOR >= 9
  if (type == "button" || type == "btn")    return lv_button_create(parent);
#else
  if (type == "button" || type == "btn")    return lv_btn_create(parent);
#endif
#if LV_USE_SLIDER
  if (type == "slider")                     return lv_slider_create(parent);
#endif
#if LV_USE_SWITCH
  if (type == "switch" || type == "sw")     return lv_switch_create(parent);
#endif
#if LV_USE_CHECKBOX
  if (type == "checkbox" || type == "cb")   return lv_checkbox_create(parent);
#endif
#if LV_USE_DROPDOWN
  if (type == "dropdown" || type == "dd")   return lv_dropdown_create(parent);
#endif
#if LV_USE_TEXTAREA
  if (type == "textarea" || type == "ta")   return lv_textarea_create(parent);
#endif
#if LV_USE_ARC
  if (type == "arc")                        return lv_arc_create(parent);
#endif
#if LV_USE_BAR
  if (type == "bar")                        return lv_bar_create(parent);
#endif
#if LV_USE_IMG || LV_USE_IMAGE
#  if LVGL_VERSION_MAJOR >= 9
  if (type == "image" || type == "img")     return lv_image_create(parent);
#  else
  if (type == "image" || type == "img")     return lv_img_create(parent);
#  endif
#endif
#if LV_USE_SPINNER
#if LVGL_VERSION_MAJOR >= 9
  if (type == "spinner")                    return lv_spinner_create(parent);
#else
  if (type == "spinner")                    return lv_spinner_create(parent, 1000, 60);
#endif
#endif
  return nullptr;
}

// ---------------------------------------------------------------------------
// Property applicator
// ---------------------------------------------------------------------------

void SentioComponent::apply_properties(lv_obj_t *obj, JsonObject props) {
  // ── Position & size ────────────────────────────────────────────────────
  if (props["x"].is<int>()) lv_obj_set_x(obj, props["x"].as<int>());
  if (props["y"].is<int>()) lv_obj_set_y(obj, props["y"].as<int>());
  if (props["w"].is<int>() || props["width"].is<int>()) {
    int w = props["w"].is<int>() ? props["w"].as<int>() : props["width"].as<int>();
    lv_obj_set_width(obj, w);
  }
  if (props["h"].is<int>() || props["height"].is<int>()) {
    int h = props["h"].is<int>() ? props["h"].as<int>() : props["height"].as<int>();
    lv_obj_set_height(obj, h);
  }

  // ── Alignment ──────────────────────────────────────────────────────────
  if (props["align"].is<const char *>()) {
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
  if (props["text"].is<const char *>()) {
    const char *text = props["text"].as<const char *>();
    if (lv_obj_check_type(obj, &lv_label_class)) {
      lv_label_set_text(obj, text);
#if LV_USE_CHECKBOX
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
      lv_checkbox_set_text(obj, text);
#endif
#if LVGL_VERSION_MAJOR >= 9
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
#else
    } else if (lv_obj_check_type(obj, &lv_btn_class)) {
#endif
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
  if (props["value"].is<int>()) {
    int val = props["value"].as<int>();
#if LV_USE_SLIDER
    if      (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_value(obj, val, LV_ANIM_OFF);
    else
#endif
#if LV_USE_ARC
    if      (lv_obj_check_type(obj, &lv_arc_class))    lv_arc_set_value(obj, val);
    else
#endif
#if LV_USE_BAR
    if      (lv_obj_check_type(obj, &lv_bar_class))    lv_bar_set_value(obj, val, LV_ANIM_OFF);
#endif
    {}
  }

  // ── Range (min/max for sliders, arcs) ─────────────────────────────────
  if (props["min"].is<int>() || props["max"].is<int>()) {
    int mn = props["min"].is<int>() ? props["min"].as<int>() : 0;
    int mx = props["max"].is<int>() ? props["max"].as<int>() : 100;
#if LV_USE_SLIDER
    if      (lv_obj_check_type(obj, &lv_slider_class)) lv_slider_set_range(obj, mn, mx);
    else
#endif
#if LV_USE_ARC
    if      (lv_obj_check_type(obj, &lv_arc_class))    lv_arc_set_range(obj, mn, mx);
    else
#endif
#if LV_USE_BAR
    if      (lv_obj_check_type(obj, &lv_bar_class))    lv_bar_set_range(obj, mn, mx);
#endif
    {}
  }

  // ── Checked state ──────────────────────────────────────────────────────
  if (props["checked"].is<bool>()) {
    if (props["checked"].as<bool>()) {
      lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(obj, LV_STATE_CHECKED);
    }
  }

  // ── Visibility ─────────────────────────────────────────────────────────
  if (props["hidden"].is<bool>()) {
    if (props["hidden"].as<bool>()) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // ── Scrollable ─────────────────────────────────────────────────────────
  if (props["scrollable"].is<bool>()) {
    if (props["scrollable"].as<bool>()) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else {
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
  }

  // ── Styles — colors ────────────────────────────────────────────────────
  if (props["bg_color"].is<const char *>()) {
    lv_color_t c = parse_hex_color(props["bg_color"].as<const char *>());
    lv_style_selector_t bg_sel = (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, c, bg_sel);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, bg_sel);
  }
  if (props["text_color"].is<const char *>()) {
    lv_color_t c = parse_hex_color(props["text_color"].as<const char *>());
    lv_obj_set_style_text_color(obj, c, (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT));
  }
  if (props["border_color"].is<const char *>()) {
    lv_color_t c = parse_hex_color(props["border_color"].as<const char *>());
    lv_obj_set_style_border_color(obj, c, (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT));
  }

  // ── Styles — geometry ──────────────────────────────────────────────────
  if (props["radius"].is<int>()) {
    lv_obj_set_style_radius(obj, props["radius"].as<int>(), LV_PART_MAIN);
  }
  if (props["border_width"].is<int>()) {
    lv_obj_set_style_border_width(obj, props["border_width"].as<int>(), LV_PART_MAIN);
  }
  if (props["pad"].is<int>()) {
    lv_obj_set_style_pad_all(obj, props["pad"].as<int>(), LV_PART_MAIN);
  }
  if (props["pad_top"].is<int>())    lv_obj_set_style_pad_top(obj,    props["pad_top"].as<int>(),    LV_PART_MAIN);
  if (props["pad_bottom"].is<int>()) lv_obj_set_style_pad_bottom(obj, props["pad_bottom"].as<int>(), LV_PART_MAIN);
  if (props["pad_left"].is<int>())   lv_obj_set_style_pad_left(obj,   props["pad_left"].as<int>(),   LV_PART_MAIN);
  if (props["pad_right"].is<int>())  lv_obj_set_style_pad_right(obj,  props["pad_right"].as<int>(),  LV_PART_MAIN);

  // ── Opacity ────────────────────────────────────────────────────────────
  if (props["opacity"].is<int>() || props["opa"].is<int>()) {
    int opa = props["opacity"].is<int>() ? props["opacity"].as<int>() : props["opa"].as<int>();
    lv_obj_set_style_opa(obj, (lv_opa_t)opa, LV_PART_MAIN);
  }

  // ── Image source path ──────────────────────────────────────────────────
#if LV_USE_IMG || LV_USE_IMAGE
  if (props["src"].is<const char *>()) {
#  if LVGL_VERSION_MAJOR >= 9
    if (lv_obj_check_type(obj, &lv_image_class)) lv_image_set_src(obj, props["src"].as<const char *>());
#  else
    if (lv_obj_check_type(obj, &lv_img_class))   lv_img_set_src(obj,   props["src"].as<const char *>());
#  endif
  }
#endif

  // ── Dropdown options ───────────────────────────────────────────────────
#if LV_USE_DROPDOWN
  if (props["options"].is<const char *>() && lv_obj_check_type(obj, &lv_dropdown_class)) {
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

  // Add support for long press
  lv_obj_add_event_cb(obj, sentio_widget_event_cb, LV_EVENT_LONG_PRESSED, data);

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
  } else if (code == LV_EVENT_LONG_PRESSED) {
    event_type = "long_pressed";
    value_str = "";
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    event_type = "value_changed";
#if LV_USE_SLIDER
    if (lv_obj_check_type(obj, &lv_slider_class)) {
      value_str = to_string(lv_slider_get_value(obj));
    } else
#endif
#if LV_USE_ARC
    if (lv_obj_check_type(obj, &lv_arc_class)) {
      value_str = to_string(lv_arc_get_value(obj));
    } else
#endif
#if LV_USE_SWITCH
    if (lv_obj_check_type(obj, &lv_switch_class)) {
      value_str = lv_obj_has_state(obj, LV_STATE_CHECKED) ? "true" : "false";
    } else
#endif
#if LV_USE_DROPDOWN
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
      char buf[64] = {0};
      lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
      value_str = buf;
    } else
#endif
    {
      // Unknown widget type — leave value_str empty
    }
  } else {
    return; // Not an event we report
  }

  // Log event locally for easy hardware/HMI debugging
  ESP_LOGI(TAG, "Widget '%s' event: %s (value: %s)", widget_id.c_str(), event_type.c_str(), value_str.c_str());

  // Fire Home Assistant event: esphome.<node>_sentio_event
  fire_homeassistant_event("esphome.sentio_event", {
    {"widget_id", widget_id},
    {"event",     event_type},
    {"value",     value_str},
  });

  // Reset idle timer on any widget interaction
  last_activity_ms_ = millis();
}

// ---------------------------------------------------------------------------
// Layout loader — resolves path to SD or LittleFS automatically
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
    if (raw_line.length() > 2) {
      parse_jsonl_line(raw_line.c_str());
    }
  }
  f.close();
#else
#ifdef USE_SENTIO_SD
  std::string resolved = resolve_path(path, sd_manager_.is_mounted());
#else
  std::string resolved = resolve_path(path, false);
#endif
  FILE *f = fopen(resolved.c_str(), "r");
  if (f == nullptr) {
    ESP_LOGW(TAG, "Layout file not found: %s", resolved.c_str());
    return;
  }
  ESP_LOGI(TAG, "Loading layout from %s", resolved.c_str());

  char buf[256];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    std::string raw_line(buf);
    // Strip trailing CR/LF
    while (!raw_line.empty() && (raw_line.back() == '\n' || raw_line.back() == '\r'))
      raw_line.pop_back();
    // Trim leading/trailing whitespace
    size_t first = raw_line.find_first_not_of(" \t");
    size_t last  = raw_line.find_last_not_of(" \t");
    if (first == std::string::npos) { raw_line.clear(); }
    else                            { raw_line = raw_line.substr(first, last - first + 1); }
    if (raw_line.length() > 2)
      parse_jsonl_line(raw_line);
  }
  fclose(f);
#endif
}

// ---------------------------------------------------------------------------
// SD card query helpers — exposed to template sensor / binary_sensor lambdas
// ---------------------------------------------------------------------------

#ifdef USE_SENTIO_SD
extern "C" {
#include "ff.h"
}
static bool sd_fatfs_info_(FATFS **fs, DWORD *free_clust, DWORD *total_clust, DWORD *sect_size) {
  FATFS *fs_ptr = nullptr;
  FRESULT res = f_getfree("/sdcard", free_clust, &fs_ptr);
  if (res != FR_OK || fs_ptr == nullptr) return false;
  *total_clust = (fs_ptr->n_fatent - 2);
  *sect_size = fs_ptr->ssize;
  *fs = fs_ptr;
  return true;
}
#endif

float SentioComponent::get_sd_free_mb() const {
#ifdef USE_SENTIO_SD
  if (!sd_manager_.is_mounted()) return NAN;
  DWORD free_clust = 0, total_clust = 0, sect_size = 0;
  FATFS *fs = nullptr;
  if (!sd_fatfs_info_(&fs, &free_clust, &total_clust, &sect_size)) return NAN;
  uint64_t free_bytes = (uint64_t)free_clust * sect_size;
  return static_cast<float>(free_bytes) / (1024.0f * 1024.0f);
#else
  return NAN;
#endif
}

float SentioComponent::get_sd_total_mb() const {
#ifdef USE_SENTIO_SD
  if (!sd_manager_.is_mounted()) return NAN;
  DWORD free_clust = 0, total_clust = 0, sect_size = 0;
  FATFS *fs = nullptr;
  if (!sd_fatfs_info_(&fs, &free_clust, &total_clust, &sect_size)) return NAN;
  uint64_t total_bytes = (uint64_t)total_clust * sect_size;
  return static_cast<float>(total_bytes) / (1024.0f * 1024.0f);
#else
  return NAN;
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
#if LV_USE_CHECKBOX
      } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_checkbox_set_text(obj, buf);
#endif
#if LVGL_VERSION_MAJOR >= 9
      } else if (lv_obj_check_type(obj, &lv_button_class)) {
#else
      } else if (lv_obj_check_type(obj, &lv_btn_class)) {
#endif
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
#if LV_USE_CHECKBOX
      } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_checkbox_set_text(obj, buf);
#endif
#if LVGL_VERSION_MAJOR >= 9
      } else if (lv_obj_check_type(obj, &lv_button_class)) {
#else
      } else if (lv_obj_check_type(obj, &lv_btn_class)) {
#endif
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

void SentioComponent::service_set_bg_image(std::string widget_id, std::string path) {
  auto it = this->widgets_.find(widget_id);
  if (it == this->widgets_.end() || it->second == nullptr) {
    ESP_LOGW(TAG, "set_bg_image: widget '%s' not found", widget_id.c_str());
    return;
  }
  lv_obj_t *obj = it->second;
  
  std::string lvgl_path = path;
  if (lvgl_path.rfind("L:", 0) != 0) {
    if (!lvgl_path.empty() && lvgl_path[0] == '/') {
      lvgl_path = "L:" + lvgl_path;
    } else {
      lvgl_path = "L:/" + lvgl_path;
    }
  }

  ESP_LOGD(TAG, "Setting background image of widget '%s' to '%s'", widget_id.c_str(), lvgl_path.c_str());

#if LV_USE_IMG || LV_USE_IMAGE
#  if LVGL_VERSION_MAJOR >= 9
  if (lv_obj_check_type(obj, &lv_image_class)) {
    lv_image_set_src(obj, lvgl_path.c_str());
    lv_obj_invalidate(lv_obj_get_parent(obj));
  } else {
    ESP_LOGW(TAG, "set_bg_image: widget '%s' is not an image widget", widget_id.c_str());
  }
#  else
  if (lv_obj_check_type(obj, &lv_img_class)) {
    lv_img_set_src(obj, lvgl_path.c_str());
    lv_obj_invalidate(lv_obj_get_parent(obj));
  } else {
    ESP_LOGW(TAG, "set_bg_image: widget '%s' is not an image widget", widget_id.c_str());
  }
#  endif
#else
  ESP_LOGW(TAG, "set_bg_image: image/img component is not enabled in this build");
#endif
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

void SentioComponent::hook_input_devices() {
  if (this->input_devices_hooked_) return;

  lv_indev_t *indev = lv_indev_get_next(nullptr);
  while (indev != nullptr) {
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
      lv_indev_read_cb_t orig_cb = lv_indev_get_read_cb(indev);
      if (orig_cb != nullptr && orig_cb != SentioComponent::indev_read_cb_wrapper) {
        this->original_read_cb_ = orig_cb;
        this->original_indev_ = indev;
        lv_indev_set_read_cb(indev, SentioComponent::indev_read_cb_wrapper);
        
        // Try to apply the long press time to LVGL's input device as well
#if LVGL_VERSION_MAJOR >= 9
        lv_indev_set_long_press_time(indev, this->long_press_time_ms_);
#endif
        
        this->input_devices_hooked_ = true;
        ESP_LOGI(TAG, "SentIO: Successfully hooked pointer input device read_cb");
        break;
      }
    }
    indev = lv_indev_get_next(indev);
  }
}

void SentioComponent::indev_read_cb_wrapper(lv_indev_t *indev, lv_indev_data_t *data) {
  if (SentioComponent::instance != nullptr) {
    SentioComponent::instance->handle_indev_read(indev, data);
  }
}

void SentioComponent::handle_indev_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (this->original_read_cb_ != nullptr) {
    this->original_read_cb_(indev, data);
  }

  uint32_t now = millis();

  // Reset activity timer on touch press
  if (data->state == LV_INDEV_STATE_PR) {
    this->last_activity_ms_ = now;
  }

  // Handle sleep wake-up
  if (data->state == LV_INDEV_STATE_PR) {
    if (this->is_sleeping_) {
      this->wake_up();
      if (this->suppress_wake_click_) {
        this->ignore_wake_tap_ = true;
        this->touch_state_ = TouchState::SWIPE; // Using SWIPE to suppress this entire gesture
      }
    }
  }

  // Handle gestures and custom state machine
  if (data->state == LV_INDEV_STATE_PR) {
    if (this->touch_state_ == TouchState::IDLE) {
      this->touch_state_ = TouchState::START;
      this->touch_start_x_ = data->point.x;
      this->touch_start_y_ = data->point.y;
      this->touch_start_ms_ = now;
    } else if (this->touch_state_ == TouchState::START) {
      int16_t dx = data->point.x - this->touch_start_x_;
      int16_t dy = data->point.y - this->touch_start_y_;

      // Detect gesture displacement
      if (std::abs(dx) >= this->gesture_threshold_px_ || std::abs(dy) >= this->gesture_threshold_px_) {
        if (now - this->touch_start_ms_ <= this->gesture_timeout_ms_) {
          this->touch_state_ = TouchState::SWIPE;
          if (std::abs(dx) >= std::abs(dy)) {
            this->handle_gesture(dx > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT);
          } else {
            this->handle_gesture(dy > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP);
          }
        } else {
          this->touch_state_ = TouchState::DRAGGING;
        }
      }
      // Detect long press timeout
      else if (now - this->touch_start_ms_ >= this->long_press_time_ms_) {
        this->touch_state_ = TouchState::LONG_PRESS;
        ESP_LOGD(TAG, "Touch classified as LONG_PRESS");
      }
    }
  } else {
    // Finger released (LV_INDEV_STATE_REL)
    if (this->touch_state_ == TouchState::LONG_PRESS || this->touch_state_ == TouchState::SWIPE) {
      int32_t hor_res = 320;
      int32_t ver_res = 240;
#if LVGL_VERSION_MAJOR >= 9
      lv_display_t *disp = lv_display_get_default();
      if (disp != nullptr) {
        hor_res = lv_display_get_horizontal_resolution(disp);
        ver_res = lv_display_get_vertical_resolution(disp);
      }
#else
      lv_disp_t *disp = lv_disp_get_default();
      if (disp != nullptr) {
        hor_res = lv_disp_get_hor_res(disp);
        ver_res = lv_disp_get_ver_res(disp);
      }
#endif
      data->state = LV_INDEV_STATE_REL;
      data->point.x = (this->touch_start_x_ < hor_res / 2) ? hor_res - 1 : 0;
      data->point.y = (this->touch_start_y_ < ver_res / 2) ? ver_res - 1 : 0;
      if (this->touch_state_ == TouchState::LONG_PRESS) {
        ESP_LOGD(TAG, "Suppressing click after long press");
      }
    }
    this->touch_state_ = TouchState::IDLE;
    this->ignore_wake_tap_ = false;
  }

  // Force suppress coordinates if currently in SWIPE state
  if (this->touch_state_ == TouchState::SWIPE) {
    int32_t hor_res = 320;
    int32_t ver_res = 240;
#if LVGL_VERSION_MAJOR >= 9
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr) {
      hor_res = lv_display_get_horizontal_resolution(disp);
      ver_res = lv_display_get_vertical_resolution(disp);
    }
#else
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != nullptr) {
      hor_res = lv_disp_get_hor_res(disp);
      ver_res = lv_disp_get_ver_res(disp);
    }
#endif
    data->state = LV_INDEV_STATE_REL;
    data->point.x = (this->touch_start_x_ < hor_res / 2) ? hor_res - 1 : 0;
    data->point.y = (this->touch_start_y_ < ver_res / 2) ? ver_res - 1 : 0;
  }
}

}  // namespace sentio
}  // namespace esphome
