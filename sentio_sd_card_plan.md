# SentIO — Native SD Card Support: Implementation Plan

**Status:** Brainstorm / Design  
**Scope:** ESP-IDF only (framework: `esp-idf`)  
**Target hardware:** ESP32 / ESP32-S3 (SDMMC peripheral) + all variants (SPI fallback)

---
pinout:

SD pad 1 = D3   (also CD/CS)          GND
SD pad 2 = CMD  (MOSI in SPI mode)    GPIO10
SD pad 3 = VSS  → GND                 ???
SD pad 4 = VDD  → 3.3V                3.3V
SD pad 5 = CLK                        GPIO12
SD pad 6 = VSS  → GND                 GND
SD pad 7 = D0   (MISO in SPI mode)    GPIO13
SD pad 8 = D1                         ???
SD pad 9 = D2                         GND


## 1. Problem Summary

SentIO currently loads and saves layouts via LittleFS (internal flash). The path routing is:

| Framework  | Read path        | Write path         | LVGL driver letter |
|------------|------------------|--------------------|--------------------|
| Arduino    | `LittleFS.open()` | `LittleFS.open()` | `L:`               |
| ESP-IDF    | `fopen(/littlefs/…)` | `fopen(/littlefs/…)` | `L:` (same POSIX layer) |

Adding SD card support means:

1. Mounting the SD card as a second VFS endpoint at `/sdcard`.
2. Registering a second LVGL FS driver letter (`S:`) that routes to `/sdcard`.
3. Letting paths in YAML and HA services resolve to SD automatically when they start with `/sdcard/` or `S:`, while falling back to LittleFS otherwise.
4. Enabling FatFS Long Filename (LFN) support so the SD card mounts correctly for files whose names exceed 8.3 characters.

---

## 2. Architecture Decision: Integrate or separate component?

**Option A — Separate sub-component** (`sentio_sd`): Clean ESPHome style, but requires the user to declare a second component ID and wire it to SentIO via `sd_card_id:`. More boilerplate.

**Option B — Inline into SentIO** (recommended for brainstorm): Add an optional `sd_card:` sub-schema block inside the existing `sentio:` YAML key. No extra component ID. Keeps all SD logic owned by SentIO.

Option B is proposed here because SentIO's SD integration is tight (LVGL driver, path routing, layout loading) and doesn't need to be reusable outside SentIO.

---

## 3. New File Layout

```
components/sentio/
├── sentio.h            ← add SD setters/members, declare sd_fs_drv
├── sentio.cpp          ← add 'S' LVGL driver, smart path router
├── sentio_sd.h         ← NEW: SdCardManager (mount/unmount/status)
├── sentio_sd.cpp       ← NEW: ESP-IDF SDMMC + SPI mount implementation
└── __init__.py         ← add sd_card: sub-schema + sdkconfig options
```

---

## 4. Phase 1 — FatFS / sdkconfig options

These must be set for LFN to work. SentIO's Python codegen injects them at
build time via `cg.add_idf_sdkconfig_option()`.

```python
# In __init__.py → to_code(), inside `if CONF_SD_CARD in config:`
if CORE.using_esp_idf:
    cg.add_idf_sdkconfig_option("CONFIG_FATFS_LFN_HEAP",     "y")
    cg.add_idf_sdkconfig_option("CONFIG_FATFS_CODEPAGE_437", "y")
    # Increase max open files for concurrent LVGL + service access
    cg.add_idf_sdkconfig_option("CONFIG_FATFS_FS_LOCK",      "5")
```

> **Why `LFN_HEAP` not `LFN_STACK`?** Stack LFN uses ~600 B of task stack per
> call. ESPHome's main loop task has limited stack. Heap allocation is safer
> even though it costs a tiny malloc per open call.

> **Why `CODEPAGE_437`?** US-ASCII superset; covers all printable characters
> used in layout file names. Adjust to `850` (Latin-1) if you need accented
> characters in filenames.

The user's existing YAML `esp32: framework: sdkconfig_options:` block also works
as an alternative to codegen injection — document both paths.

---

## 5. Phase 2 — `sentio_sd.h` / `sentio_sd.cpp`

### 5.1 `sentio_sd.h`

```cpp
#pragma once
#ifdef USE_ESP_IDF

#include "esphome/core/component.h"

namespace esphome {
namespace sentio {

// Interface modes
enum class SdMode : uint8_t { SDMMC_1BIT, SDMMC_4BIT, SPI };

struct SdCardConfig {
  SdMode   mode{SdMode::SDMMC_4BIT};
  int8_t   clk_pin{-1};
  int8_t   cmd_pin{-1};   // MOSI for SPI
  int8_t   data0_pin{-1}; // MISO for SPI
  int8_t   data1_pin{-1};
  int8_t   data2_pin{-1};
  int8_t   data3_pin{-1}; // CS for SPI
  bool     format_if_mount_failed{false};
};

// Mounts/unmounts the SD card VFS. Owned by SentioComponent.
class SdCardManager {
 public:
  bool mount(const SdCardConfig &cfg);
  void unmount();
  bool is_mounted() const { return mounted_; }
  const char *mount_point() const { return "/sdcard"; }

 private:
  bool mounted_{false};
  void *card_{nullptr}; // sdmmc_card_t* — opaque here to avoid IDF header bleed
};

}  // namespace sentio
}  // namespace esphome

#endif  // USE_ESP_IDF
```

### 5.2 `sentio_sd.cpp` — mount logic

```cpp
#ifdef USE_ESP_IDF
#include "sentio_sd.h"
#include "esphome/core/log.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"

static const char *const TAG = "sentio.sd";
static constexpr const char *MOUNT_POINT = "/sdcard";

namespace esphome {
namespace sentio {

bool SdCardManager::mount(const SdCardConfig &cfg) {
  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
    .format_if_mount_failed = cfg.format_if_mount_failed,
    .max_files              = 5,
    .allocation_unit_size   = 16 * 1024,
  };

  sdmmc_card_t *card = nullptr;
  esp_err_t ret;

  if (cfg.mode == SdMode::SPI) {
    ret = mount_spi_(cfg, mount_cfg, &card);
  } else {
    ret = mount_sdmmc_(cfg, mount_cfg, &card);
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
    return false;
  }

  card_ = card;
  mounted_ = true;
  ESP_LOGI(TAG, "SD card mounted at %s (%.0f MB)",
           MOUNT_POINT,
           (float)((sdmmc_card_t *)card_)->csd.capacity * 512 / (1024 * 1024));
  return true;
}

void SdCardManager::unmount() {
  if (!mounted_) return;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, (sdmmc_card_t *)card_);
  mounted_ = false;
  card_    = nullptr;
}

// ── Private helpers ───────────────────────────────────────────────────────

esp_err_t SdCardManager::mount_sdmmc_(const SdCardConfig &cfg,
                                       esp_vfs_fat_sdmmc_mount_config_t &mount_cfg,
                                       sdmmc_card_t **out_card) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

  slot.width = (cfg.mode == SdMode::SDMMC_1BIT) ? 1 : 4;

#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  // ESP32-S3 and newer: pins are fully configurable
  slot.clk = static_cast<gpio_num_t>(cfg.clk_pin);
  slot.cmd = static_cast<gpio_num_t>(cfg.cmd_pin);
  slot.d0  = static_cast<gpio_num_t>(cfg.data0_pin);
  if (cfg.mode == SdMode::SDMMC_4BIT) {
    slot.d1 = static_cast<gpio_num_t>(cfg.data1_pin);
    slot.d2 = static_cast<gpio_num_t>(cfg.data2_pin);
    slot.d3 = static_cast<gpio_num_t>(cfg.data3_pin);
  }
#endif
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, out_card);
}

esp_err_t SdCardManager::mount_spi_(const SdCardConfig &cfg,
                                     esp_vfs_fat_sdmmc_mount_config_t &mount_cfg,
                                     sdmmc_card_t **out_card) {
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev_cfg.gpio_cs   = static_cast<gpio_num_t>(cfg.data3_pin); // CS
  dev_cfg.host_id   = static_cast<spi_host_device_t>(host.slot);

  return esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_cfg, &mount_cfg, out_card);
}

}  // namespace sentio
}  // namespace esphome
#endif // USE_ESP_IDF
```

> **Key implementation notes:**
> - `SOC_SDMMC_USE_GPIO_MATRIX` is defined on ESP32-S3 (flexible pin routing).
>   On classic ESP32, the SDMMC peripheral has fixed pins (CLK=14, CMD=15, D0=2,
>   D1=4, D2=12, D3=13) — the slot struct does NOT have pin fields, so the
>   `#ifdef` block is skipped.
> - SPI mode uses the `data3_pin` as CS (matches the ESPHome convention of
>   listing data pins 0-3, where pin 3 doubles as CS in SPI).
> - Both paths end up calling the FatFS/VFS layer, so POSIX `fopen()` works
>   identically regardless of physical interface.

---

## 6. Phase 3 — LVGL `S:` driver in `sentio.cpp`

The existing `L:` driver routes everything to `/littlefs`. A second static driver
letter `S:` routes to `/sdcard`. The POSIX FILE* layer is identical — only the
path prefix changes.

```cpp
// In sentio.cpp — alongside the existing sentio_fs_drv

static lv_fs_drv_t sentio_sd_fs_drv;

static void *sentio_sd_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
  const char *mode_str = (mode == LV_FS_MODE_WR) ? "w" : "r";
  std::string full = "/sdcard";
  if (path[0] != '/') full += '/';
  full += path;
  FILE *f = fopen(full.c_str(), mode_str);
  return static_cast<void *>(f);
}

// close/read/write/seek/tell callbacks are IDENTICAL to the L: driver.
// Extract them into shared static helpers to avoid duplication:

static lv_fs_res_t sentio_fs_close_impl(lv_fs_drv_t *, void *fp) { … }
static lv_fs_res_t sentio_fs_read_impl(lv_fs_drv_t *, void *fp, …)  { … }
// etc.
```

Then in `SentioComponent::setup()`, after registering `L:`:

```cpp
#ifdef USE_SENTIO_SD
  if (sd_manager_.is_mounted()) {
    lv_fs_drv_init(&sentio_sd_fs_drv);
    sentio_sd_fs_drv.letter   = 'S';
    sentio_sd_fs_drv.open_cb  = sentio_sd_fs_open;
    sentio_sd_fs_drv.close_cb = sentio_fs_close_impl;
    sentio_sd_fs_drv.read_cb  = sentio_fs_read_impl;
    sentio_sd_fs_drv.write_cb = sentio_fs_write_impl;
    sentio_sd_fs_drv.seek_cb  = sentio_fs_seek_impl;
    sentio_sd_fs_drv.tell_cb  = sentio_fs_tell_impl;
    lv_fs_drv_register(&sentio_sd_fs_drv);
    ESP_LOGI(TAG, "LVGL SD driver registered (S:)");
  }
#endif
```

> The `USE_SENTIO_SD` define is emitted by Python codegen when `sd_card:` is
> present in YAML (via `cg.add_define("USE_SENTIO_SD")`).

---

## 7. Phase 4 — Smart path routing

The existing path-resolution logic for `load_layout_from_file()` and
`service_save_layout_line()` needs to understand the SD mount point.

**Priority order (proposed):**

```
1. Path starts with "/sdcard/"  → open on SD directly (no fallback)
2. Path starts with "/littlefs/" → open on LittleFS directly
3. Bare path (e.g. "/sentio.jsonl") → try SD first (if mounted), then LittleFS
```

```cpp
// Utility helper (replaces ad-hoc prefix checks scattered through the file)
static std::string resolve_storage_path(const std::string &path, bool sd_mounted) {
  if (path.rfind("/sdcard", 0) == 0)  return path;              // explicit SD
  if (path.rfind("/littlefs", 0) == 0) return path;             // explicit LFS
  // bare path: prefer SD if available
  std::string bare = (path.empty() || path[0] != '/') ? "/" + path : path;
  if (sd_mounted) return "/sdcard" + bare;
  return "/littlefs" + bare;
}
```

This replaces the existing three separate prefix-check blocks in `sentio_fs_open`,
`service_save_layout_line`, and `load_layout_from_file`.

---

## 8. Phase 5 — `__init__.py` changes

### 8.1 New YAML constants

```python
CONF_SD_CARD        = "sd_card"
CONF_SD_MODE        = "mode"
CONF_SD_CLK_PIN     = "clk_pin"
CONF_SD_CMD_PIN     = "cmd_pin"
CONF_SD_DATA0_PIN   = "data0_pin"
CONF_SD_DATA1_PIN   = "data1_pin"
CONF_SD_DATA2_PIN   = "data2_pin"
CONF_SD_DATA3_PIN   = "data3_pin"
CONF_SD_FORMAT_FAIL = "format_if_mount_failed"
```

### 8.2 Sub-schema

```python
SdMode = sentio_ns.enum("SdMode")
SD_MODE_OPTIONS = {
    "sdmmc_1bit": SdMode.SDMMC_1BIT,
    "sdmmc_4bit": SdMode.SDMMC_4BIT,
    "spi":        SdMode.SPI,
}

SD_CARD_SCHEMA = cv.Schema({
    cv.Optional(CONF_SD_MODE, default="sdmmc_4bit"): cv.enum(SD_MODE_OPTIONS),
    cv.Required(CONF_SD_CLK_PIN):   cv.int_range(min=0, max=48),
    cv.Required(CONF_SD_CMD_PIN):   cv.int_range(min=0, max=48),
    cv.Required(CONF_SD_DATA0_PIN): cv.int_range(min=0, max=48),
    cv.Optional(CONF_SD_DATA1_PIN): cv.int_range(min=0, max=48),
    cv.Optional(CONF_SD_DATA2_PIN): cv.int_range(min=0, max=48),
    cv.Optional(CONF_SD_DATA3_PIN): cv.int_range(min=0, max=48),
    cv.Optional(CONF_SD_FORMAT_FAIL, default=False): cv.boolean,
})
```

Add to `CONFIG_SCHEMA`:
```python
cv.Optional(CONF_SD_CARD): SD_CARD_SCHEMA,
```

### 8.3 `to_code()` additions

```python
if CONF_SD_CARD in config:
    sd = config[CONF_SD_CARD]
    cg.add_define("USE_SENTIO_SD")

    # Inject sdkconfig options for FatFS LFN
    if CORE.using_esp_idf:
        cg.add_idf_sdkconfig_option("CONFIG_FATFS_LFN_HEAP",     "y")
        cg.add_idf_sdkconfig_option("CONFIG_FATFS_CODEPAGE_437", "y")
        cg.add_idf_sdkconfig_option("CONFIG_FATFS_FS_LOCK",      "5")

    cg.add(var.set_sd_mode(sd[CONF_SD_MODE]))
    cg.add(var.set_sd_clk_pin(sd[CONF_SD_CLK_PIN]))
    cg.add(var.set_sd_cmd_pin(sd[CONF_SD_CMD_PIN]))
    cg.add(var.set_sd_data0_pin(sd[CONF_SD_DATA0_PIN]))
    if CONF_SD_DATA1_PIN in sd:
        cg.add(var.set_sd_data1_pin(sd[CONF_SD_DATA1_PIN]))
    if CONF_SD_DATA2_PIN in sd:
        cg.add(var.set_sd_data2_pin(sd[CONF_SD_DATA2_PIN]))
    if CONF_SD_DATA3_PIN in sd:
        cg.add(var.set_sd_data3_pin(sd[CONF_SD_DATA3_PIN]))
    cg.add(var.set_sd_format_if_mount_failed(sd[CONF_SD_FORMAT_FAIL]))
```

### 8.4 Validation: mode vs. pin completeness

Add a Python validator to catch incomplete pin configs at YAML parse time:

```python
def _validate_sd_config(config):
    if CONF_SD_CARD not in config:
        return config
    sd = config[CONF_SD_CARD]
    mode = sd.get(CONF_SD_MODE, "sdmmc_4bit")
    if mode == "sdmmc_4bit":
        for pin in (CONF_SD_DATA1_PIN, CONF_SD_DATA2_PIN, CONF_SD_DATA3_PIN):
            if pin not in sd:
                raise cv.Invalid(f"4-bit SDMMC requires {pin}")
    if mode == "spi":
        if CONF_SD_DATA3_PIN not in sd:
            raise cv.Invalid("SPI mode uses data3_pin as CS — must be specified")
    return config

CONFIG_SCHEMA = cv.All(_validate_sd_config, CONFIG_SCHEMA)
```

---

## 9. Phase 6 — `sentio.h` additions

```cpp
#ifdef USE_SENTIO_SD
#  include "sentio_sd.h"
#endif

class SentioComponent : public Component, public api::CustomAPIDevice {
 public:
  // … existing …

#ifdef USE_SENTIO_SD
  void set_sd_mode(SdMode m)               { sd_cfg_.mode = m; }
  void set_sd_clk_pin(int8_t p)            { sd_cfg_.clk_pin = p; }
  void set_sd_cmd_pin(int8_t p)            { sd_cfg_.cmd_pin = p; }
  void set_sd_data0_pin(int8_t p)          { sd_cfg_.data0_pin = p; }
  void set_sd_data1_pin(int8_t p)          { sd_cfg_.data1_pin = p; }
  void set_sd_data2_pin(int8_t p)          { sd_cfg_.data2_pin = p; }
  void set_sd_data3_pin(int8_t p)          { sd_cfg_.data3_pin = p; }
  void set_sd_format_if_mount_failed(bool v){ sd_cfg_.format_if_mount_failed = v; }
#endif

 private:
#ifdef USE_SENTIO_SD
  SdCardConfig  sd_cfg_;
  SdCardManager sd_manager_;
#endif
};
```

---

## 10. Phase 7 — YAML example (user-facing)

```yaml
external_components:
  - source: github://yourusername/sentio@main
    components: [sentio]

esp32:
  board: esp32s3dev
  framework:
    type: esp-idf
    # Alternative to codegen injection — explicit sdkconfig override:
    sdkconfig_options:
      CONFIG_FATFS_LFN_HEAP: "y"
      CONFIG_FATFS_CODEPAGE_437: "y"

sentio:
  id: my_sentio
  touch_source: my_touch
  backlight_output: my_backlight
  sleep_timeout: 5min
  startup_layout: /sentio.jsonl   # bare path → resolves to SD if mounted

  sd_card:
    mode: sdmmc_4bit
    clk_pin: 14
    cmd_pin: 15
    data0_pin: 2
    data1_pin: 4
    data2_pin: 12
    data3_pin: 13
    format_if_mount_failed: false

  on_wake:
    - logger.log: "Woke up"
```

For ESP32-S3 (e.g. the Waveshare 4.3" display board):
```yaml
  sd_card:
    mode: sdmmc_4bit
    clk_pin: 40   # board-specific
    cmd_pin: 38
    data0_pin: 39
    data1_pin: 41
    data2_pin: 42
    data3_pin: 43
```

---

## 11. Caveats & open questions

| Topic | Decision needed |
|-------|-----------------|
| **Classic ESP32 fixed pins** | On original ESP32, SDMMC slot 1 has hardwired pins. The `clk_pin` / `cmd_pin` / `data*_pin` setters in the schema become informational only (used for `dump_config()`) and should be validated against the expected values. Add a Python warning if pins don't match? |
| **SPI bus sharing** | If the display is already on the SPI bus, `sdspi_host` needs the same SPI host ID (`host_id`). Extend the SPI config with a `spi_id:` reference to an ESPHome `spi:` component. |
| **Arduino framework** | The `sentio_sd.cpp` guard is `#ifdef USE_ESP_IDF`. For Arduino, the SD library is an option but it can't use POSIX fopen — it would need a separate Arduino-specific LVGL open callback. Not in scope for Phase 1; add `DEPENDENCIES: ["esp_idf"]` guard in Python codegen (raise `Invalid` if Arduino). |
| **Hot-swap / card absent** | `esp_vfs_fat_sdmmc_mount` fails gracefully (returns `ESP_ERR_NO_MEM` or `ESP_FAIL`). SentIO should call `mark_failed()` if SD is configured but absent — or warn and fall back to LittleFS depending on the use case. |
| **Thread safety** | LVGL FS callbacks and HA service calls both write to `/sdcard`. FatFS `CONFIG_FATFS_FS_LOCK=5` enables re-entrancy. Ensure HA service calls that open files do so only from the main ESPHome loop (not from a different task). |
| **File path max length** | `ESP_VFS_PATH_MAX` = 15 + mount point. With LFN enabled, `CONFIG_FATFS_MAX_LFN` = 255. Use `char buf[ESP_VFS_PATH_MAX + 256]` for path construction, not a fixed small buffer. |
| **`service_set_bg_image` + LVGL** | `lv_img_set_src(obj, "S:/images/bg.png")` requires LVGL's PNG decoder to be enabled and the path to be prefixed with the driver letter. Confirm LVGL PNG support is compiled in via `CONFIG_LV_USE_PNG=1` / `lv_conf.h`. |

---

## 12. Implementation order (suggested sprints)

```
Sprint 1  ─ sentio_sd.h + sentio_sd.cpp (SDMMC mount only)
            ─ Manual test: mount prints to UART, fopen("/sdcard/test.txt") works

Sprint 2  ─ __init__.py: sd_card sub-schema + sdkconfig injection
            ─ sentio.h: setters + SdCardManager member
            ─ sentio.cpp: call sd_manager_.mount() in setup()

Sprint 3  ─ LVGL S: driver registration
            ─ resolve_storage_path() helper replacing scattered prefix logic
            ─ load_layout_from_file() and service_save_layout_line() use helper

Sprint 4  ─ SPI mode in sentio_sd.cpp
            ─ dump_config() SD section
            ─ Python pin validator

Sprint 5  ─ service_set_bg_image path routing through S:
            ─ YAML docs + example configs for common boards
```

---

## 13. Files changed summary

| File | Change type | Notes |
|------|-------------|-------|
| `sentio_sd.h` | **NEW** | SdCardConfig struct, SdCardManager class |
| `sentio_sd.cpp` | **NEW** | SDMMC + SPI mount, ESP-IDF only |
| `sentio.h` | Modify | Add SD setters, `SdCardManager sd_manager_`, `#include sentio_sd.h` |
| `sentio.cpp` | Modify | Add S: LVGL driver, `resolve_storage_path()`, call `sd_manager_.mount()` |
| `__init__.py` | Modify | `sd_card:` sub-schema, sdkconfig options, Python pin validator |
| `component.json` | No change | External deps handled via ESP-IDF native components |
