#include "sentio_sd.h"

#ifdef USE_SENTIO_SD

#include "esphome/core/log.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"   // spi_bus_initialize / spi_bus_free

static const char *const TAG = "sentio.sd";

namespace esphome {
namespace sentio {

static constexpr const char *MOUNT_POINT = "/sdcard";

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SdCardManager::mount(const SdCardConfig &cfg) {
  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
    .format_if_mount_failed = cfg.format_if_mount_failed,
    .max_files              = 5,
    .allocation_unit_size   = 16 * 1024,
  };

  sdmmc_card_t *card = nullptr;
  esp_err_t ret;

  if (cfg.mode == SdMode::SPI) {
    ret = static_cast<esp_err_t>(mount_spi_(cfg, &mount_cfg, reinterpret_cast<void **>(&card)));
  } else {
    ret = static_cast<esp_err_t>(mount_sdmmc_(cfg, &mount_cfg, reinterpret_cast<void **>(&card)));
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
    return false;
  }

  card_    = card;
  mounted_ = true;

  float capacity_mb = (float)card->csd.capacity * 512.0f / (1024.0f * 1024.0f);
  ESP_LOGI(TAG, "SD mounted at %s (%.0f MB)", MOUNT_POINT, capacity_mb);
  return true;
}

void SdCardManager::unmount() {
  if (!mounted_) return;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, static_cast<sdmmc_card_t *>(card_));
  mounted_ = false;
  card_    = nullptr;
  ESP_LOGI(TAG, "SD unmounted");
}

// ---------------------------------------------------------------------------
// SDMMC (native peripheral) mount
// ---------------------------------------------------------------------------

int SdCardManager::mount_sdmmc_(const SdCardConfig &cfg, void *mount_cfg_p, void **out_card) {
  auto *mount_cfg = static_cast<esp_vfs_fat_sdmmc_mount_config_t *>(mount_cfg_p);

  sdmmc_host_t        host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

  slot.width  = (cfg.mode == SdMode::SDMMC_1BIT) ? 1 : 4;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // ESP32-S3 and newer SoCs expose a GPIO matrix for SDMMC pins.
  // Classic ESP32 has fixed pins — slot struct has no pin fields there.
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  slot.clk = static_cast<gpio_num_t>(cfg.clk_pin);
  slot.cmd = static_cast<gpio_num_t>(cfg.cmd_pin);
  slot.d0  = static_cast<gpio_num_t>(cfg.data0_pin);
  if (cfg.mode == SdMode::SDMMC_4BIT) {
    slot.d1 = static_cast<gpio_num_t>(cfg.data1_pin);
    slot.d2 = static_cast<gpio_num_t>(cfg.data2_pin);
    slot.d3 = static_cast<gpio_num_t>(cfg.data3_pin);
  }
  ESP_LOGI(TAG, "SD SDMMC %s: CLK=GPIO%d, CMD=GPIO%d, D0=GPIO%d%s",
           (cfg.mode == SdMode::SDMMC_4BIT) ? "4-bit" : "1-bit",
           cfg.clk_pin, cfg.cmd_pin, cfg.data0_pin,
           (cfg.mode == SdMode::SDMMC_4BIT) ? " (+D1-D3)" : "");
#else
  ESP_LOGI(TAG, "SD SDMMC %s: using fixed pins (classic ESP32)",
           (cfg.mode == SdMode::SDMMC_4BIT) ? "4-bit" : "1-bit");
#endif

  return static_cast<int>(
    esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, mount_cfg,
                            reinterpret_cast<sdmmc_card_t **>(out_card)));
}

// ---------------------------------------------------------------------------
// SDSPI (SPI bus) mount
//
// Responsibilities here that callers don't handle:
//   1. Initialise the SPI host (spi_bus_initialize) — ESPHome's spi: component
//      only initialises the host it owns (SPI2 for the display).  SPI3 is
//      untouched until we claim it here.
//   2. Register the SD card as an SDSPI device on that host.
//   3. Mount FatFS via esp_vfs_fat_sdspi_mount.
//
// Hardwired-CS boards (e.g. JC3248W535):
//   D3/CS is tied directly to GND on the PCB, permanently forcing SPI mode.
//   The IDF SDSPI driver still requires a gpio_cs argument; we accept any
//   free GPIO as a "dummy CS".  The driver will toggle it, but the card
//   ignores it because the hardware CS is already asserted.
//   Pass data3_pin to that dummy GPIO in YAML (e.g. GPIO11).
// ---------------------------------------------------------------------------

int SdCardManager::mount_spi_(const SdCardConfig &cfg, void *mount_cfg_p, void **out_card) {
  auto *mount_cfg = static_cast<esp_vfs_fat_sdmmc_mount_config_t *>(mount_cfg_p);

  if (cfg.spi_host < 0) {
    ESP_LOGE(TAG, "SD SPI mode requires 'spi_host' in YAML "
                  "(1=SPI2_HOST shared bus, 2=SPI3_HOST separate bus)");
    return static_cast<int>(ESP_ERR_INVALID_ARG);
  }

  // ── Step 1: Initialise the SPI bus ───────────────────────────────────────
  // clk_pin  → SCLK
  // cmd_pin  → MOSI  (CMD pad on SD footprint)
  // data0_pin→ MISO  (D0 pad on SD footprint)
  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num     = cfg.cmd_pin;
  bus_cfg.miso_io_num     = cfg.data0_pin;
  bus_cfg.sclk_io_num     = cfg.clk_pin;
  bus_cfg.quadwp_io_num   = -1;
  bus_cfg.quadhd_io_num   = -1;
  bus_cfg.max_transfer_sz = 4096;

  esp_err_t bus_ret = spi_bus_initialize(
    static_cast<spi_host_device_t>(cfg.spi_host), &bus_cfg, SPI_DMA_CH_AUTO);

  if (bus_ret == ESP_ERR_INVALID_STATE) {
    // Already initialised by another component — acceptable, continue.
    ESP_LOGD(TAG, "SPI host %d already initialised", cfg.spi_host);
  } else if (bus_ret != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(bus_ret));
    return static_cast<int>(bus_ret);
  }

  // ── Step 2: Register SDSPI device and mount ───────────────────────────────
  sdmmc_host_t          host = SDSPI_HOST_DEFAULT();
  sdspi_device_config_t dev  = SDSPI_DEVICE_CONFIG_DEFAULT();

  host.slot   = cfg.spi_host;
  dev.host_id = static_cast<spi_host_device_t>(cfg.spi_host);
  dev.gpio_cs = static_cast<gpio_num_t>(cfg.data3_pin);

  const bool cs_is_dummy = (cfg.cs_hardwired);
  ESP_LOGI(TAG, "SD SPI: host=%d, CLK=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, CS=GPIO%d%s",
           cfg.spi_host, cfg.clk_pin, cfg.cmd_pin, cfg.data0_pin, cfg.data3_pin,
           cs_is_dummy ? " (dummy — D3 hardwired to GND on PCB)" : "");

  return static_cast<int>(
    esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, mount_cfg,
                            reinterpret_cast<sdmmc_card_t **>(out_card)));
}

}  // namespace sentio
}  // namespace esphome

#endif  // USE_SENTIO_SD
