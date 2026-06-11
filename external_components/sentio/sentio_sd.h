#pragma once

#ifdef USE_ESP_IDF

namespace esphome {
namespace sentio {

enum class SdMode : uint8_t {
  SDMMC_1BIT,
  SDMMC_4BIT,
  SPI,
};

struct SdCardConfig {
  SdMode mode{SdMode::SDMMC_4BIT};
  int8_t clk_pin{-1};
  int8_t cmd_pin{-1};    // MOSI in SPI mode
  int8_t data0_pin{-1};  // MISO in SPI mode
  int8_t data1_pin{-1};
  int8_t data2_pin{-1};
  int8_t data3_pin{-1};  // CS in SPI mode
  // SPI host index (ESP-IDF SPI2_HOST=1, SPI3_HOST=2).
  // MUST be specified when mode=SPI to avoid colliding with the display bus.
  // Unused for SDMMC modes.
  int8_t spi_host{-1};
  // Set true when the board hardwires D3/CS to GND (e.g. JC3248W535).
  // data3_pin then acts as a dummy GPIO for the IDF driver — any free pin works.
  bool   cs_hardwired{false};
  bool   format_if_mount_failed{false};
};

// Owns the SD card VFS mount lifetime.
// Call mount() once in setup(); unmount() on teardown (rarely needed).
class SdCardManager {
 public:
  bool        mount(const SdCardConfig &cfg);
  void        unmount();
  bool        is_mounted()   const { return mounted_; }
  const char *mount_point()  const { return "/sdcard"; }

 private:
  bool  mounted_{false};
  void *card_{nullptr};  // sdmmc_card_t* — opaque to avoid IDF header leakage

  // Split per interface to keep each function under 30 lines.
  int mount_sdmmc_(const SdCardConfig &cfg, void *mount_cfg_p, void **out_card);
  int mount_spi_(const SdCardConfig &cfg, void *mount_cfg_p, void **out_card);
};

}  // namespace sentio
}  // namespace esphome

#endif  // USE_ESP_IDF
