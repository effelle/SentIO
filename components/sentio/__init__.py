"""
SentIO — ESPHome Dynamic LVGL Overlay Component
Phase 1: Python configuration schema and C++ code generator.

Registers:
  - Component class SentioComponent
  - YAML options: touch_source, backlight, sleep, burn-in, sd_card, etc.
  - on_sleep / on_wake automation triggers
  - HA API services: run_jsonl, clear, load_layout, save_layout_line, ...
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import light, output, touchscreen
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE

CODEOWNERS = ["@effelle"]
DEPENDENCIES = ["api", "lvgl"]
AUTO_LOAD = ["json"]

# ---------------------------------------------------------------------------
# C++ namespace / class declarations
# ---------------------------------------------------------------------------
sentio_ns = cg.esphome_ns.namespace("sentio")

SentioComponent = sentio_ns.class_("SentioComponent", cg.Component)

# Trigger wrappers — each constructor self-registers with the parent component.
SleepTrigger = sentio_ns.class_("SleepTrigger", automation.Trigger.template())
WakeTrigger  = sentio_ns.class_("WakeTrigger",  automation.Trigger.template())

SwipeLeftTrigger  = sentio_ns.class_("SwipeLeftTrigger",  automation.Trigger.template())
SwipeRightTrigger = sentio_ns.class_("SwipeRightTrigger", automation.Trigger.template())
SwipeUpTrigger    = sentio_ns.class_("SwipeUpTrigger",    automation.Trigger.template())
SwipeDownTrigger  = sentio_ns.class_("SwipeDownTrigger",  automation.Trigger.template())

PageShowTrigger   = sentio_ns.class_("PageShowTrigger",   automation.Trigger.template(cg.std_string))
PageHideTrigger   = sentio_ns.class_("PageHideTrigger",   automation.Trigger.template(cg.std_string))

IsPageCondition   = sentio_ns.class_("IsPageCondition",   automation.Condition)

# ---------------------------------------------------------------------------
# YAML configuration key constants
# ---------------------------------------------------------------------------
CONF_TOUCH_SOURCE             = "touch_source"
CONF_BACKLIGHT_LIGHT          = "backlight_light"
CONF_BACKLIGHT_OUTPUT         = "backlight_output"
CONF_SLEEP_TIMEOUT            = "sleep_timeout"
CONF_SUPPRESS_WAKE_CLICK      = "suppress_wake_click"
CONF_ANTI_BURN_IN             = "anti_burn_in"
CONF_DISPLAY_OFF_BEFORE_SLEEP = "display_off_before_sleep"
CONF_STARTUP_LAYOUT           = "startup_layout"
CONF_SOFT_SLEEP_ONLY          = "soft_sleep_only"
CONF_GESTURE_THRESHOLD        = "gesture_threshold"
CONF_GESTURE_TIMEOUT          = "gesture_timeout"
CONF_LONG_PRESS_TIME          = "long_press_time"
CONF_ON_SLEEP                 = "on_sleep"
CONF_ON_WAKE                  = "on_wake"
CONF_ON_SWIPE_LEFT            = "on_swipe_left"
CONF_ON_SWIPE_RIGHT           = "on_swipe_right"
CONF_ON_SWIPE_UP              = "on_swipe_up"
CONF_ON_SWIPE_DOWN            = "on_swipe_down"
CONF_ON_PAGE_SHOW             = "on_page_show"
CONF_ON_PAGE_HIDE             = "on_page_hide"
CONF_PAGE_ID                  = "page_id"

# SD card
CONF_SD_CARD                  = "sentio_sd"
CONF_SD_MODE                  = "mode"
CONF_SD_CLK_PIN               = "clk_pin"
CONF_SD_CMD_PIN               = "cmd_pin"
CONF_SD_DATA0_PIN             = "data0_pin"
CONF_SD_DATA1_PIN             = "data1_pin"
CONF_SD_DATA2_PIN             = "data2_pin"
CONF_SD_DATA3_PIN             = "data3_pin"
CONF_SD_FORMAT_IF_MOUNT_FAILED = "format_if_mount_failed"
CONF_SD_SPI_HOST               = "spi_host"
CONF_SD_CS_HARDWIRED           = "cs_hardwired"

SdMode = sentio_ns.enum("SdMode")
SD_MODE_OPTIONS = {
    "sdmmc_1bit": SdMode.SDMMC_1BIT,
    "sdmmc_4bit": SdMode.SDMMC_4BIT,
    "spi":        SdMode.SPI,
}


def _sd_card_schema(value):
    """Apply the sd_card field schema then enforce cross-field pin rules.

    Keeping validation inside the sub-schema (rather than wrapping CONFIG_SCHEMA
    in cv.All) lets ESPHome introspect the top-level schema keys correctly and
    produce accurate 'invalid option' messages for unrelated fields.
    """
    value = cv.Schema({
        cv.Optional(CONF_SD_MODE, default="sdmmc_4bit"):
            cv.enum(SD_MODE_OPTIONS),
        cv.Required(CONF_SD_CLK_PIN):   cv.int_range(min=0, max=48),
        cv.Required(CONF_SD_CMD_PIN):   cv.int_range(min=0, max=48),
        cv.Required(CONF_SD_DATA0_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_SD_DATA1_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_SD_DATA2_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_SD_DATA3_PIN): cv.int_range(min=0, max=48),
        # Required when mode: spi. Must be declared explicitly — there is no
        # safe default because the right host depends on your board wiring:
        #   spi_host: 1  (SPI2_HOST) — share with a standard single-wire SPI bus
        #   spi_host: 2  (SPI3_HOST) — separate bus; mandatory on QSPI/quad-SPI boards
        # Not used for SDMMC modes.
        cv.Optional(CONF_SD_SPI_HOST): cv.int_range(min=0, max=2),
        # Set true when D3/CS is hardwired to GND on the PCB (e.g. JC3248W535).
        # In this case data3_pin is a dummy GPIO — the driver toggles it but the
        # card ignores it because the hardware CS is permanently asserted.
        cv.Optional(CONF_SD_CS_HARDWIRED, default=False): cv.boolean,
        cv.Optional(CONF_SD_FORMAT_IF_MOUNT_FAILED, default=False): cv.boolean,
    })(value)

    mode = value.get(CONF_SD_MODE, "sdmmc_4bit")

    if mode == "sdmmc_4bit":
        for pin in (CONF_SD_DATA1_PIN, CONF_SD_DATA2_PIN, CONF_SD_DATA3_PIN):
            if pin not in value:
                raise cv.Invalid(f"sd_card mode 'sdmmc_4bit' requires '{pin}'")

    if mode == "spi":
        if CONF_SD_DATA3_PIN not in value:
            raise cv.Invalid(
                "sd_card mode 'spi' uses data3_pin as CS — it must be specified"
            )
        if CONF_SD_SPI_HOST not in value:
            raise cv.Invalid(
                "sd_card mode 'spi' requires 'spi_host'.\n"
                "  spi_host: 1  → SPI2_HOST (shared with display on single-SPI boards)\n"
                "  spi_host: 2  → SPI3_HOST (separate bus; required on QSPI/quad-SPI boards)\n"
                "Check your board's SPI wiring before choosing."
            )

    return value


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SentioComponent),

    # Optional: proxy an existing ESPHome touchscreen for smart-touch features.
    # If omitted, SentIO manages the GUI layer only (JSONL / backlight / burn-in).
    cv.Optional(CONF_TOUCH_SOURCE): cv.use_id(touchscreen.Touchscreen),

    # Backlight control — use exactly ONE of these.
    cv.Optional(CONF_BACKLIGHT_LIGHT):  cv.use_id(light.LightState),
    cv.Optional(CONF_BACKLIGHT_OUTPUT): cv.use_id(output.FloatOutput),

    # Power management
    cv.Optional(CONF_SLEEP_TIMEOUT, default="60s"):
        cv.positive_time_period_milliseconds,

    # Suppress the first tap after wake so it does NOT click a widget.
    cv.Optional(CONF_SUPPRESS_WAKE_CLICK, default=True): cv.boolean,

    # Slowly shift the root container ±1 px every 60 s (OLED burn-in guard).
    cv.Optional(CONF_ANTI_BURN_IN, default=False): cv.boolean,

    # Send LVGL blank + backlight=0 BEFORE firing on_sleep.
    # Prevents the display glowing on boards where the SPI bus floats (e.g. JC3248W535C).
    cv.Optional(CONF_DISPLAY_OFF_BEFORE_SLEEP, default=True): cv.boolean,

    # LittleFS path of a .jsonl file to load automatically on boot.
    cv.Optional(CONF_STARTUP_LAYOUT, default=""): cv.string,

    # Override auto-detected soft sleep (no reset pin)
    cv.Optional(CONF_SOFT_SLEEP_ONLY): cv.boolean,

    # Gesture detection tuning (applies when touch_source is configured)
    # gesture_threshold: min pixels of displacement to register as a swipe (default 80)
    cv.Optional(CONF_GESTURE_THRESHOLD, default=80): cv.int_range(min=10, max=300),
    # gesture_timeout: max ms a touch can stay in START before it's classified as
    # a tap/long-press and gesture detection is locked out (default 300 ms).
    # Set this lower than your long_press threshold to prevent drift-swipes.
    cv.Optional(CONF_GESTURE_TIMEOUT, default="300ms"):
        cv.positive_time_period_milliseconds,
    # long_press_time: time in ms a touch must be held to register as a long press.
    # When released after a long press, SentIO suppresses the clicked event.
    cv.Optional(CONF_LONG_PRESS_TIME, default="500ms"):
        cv.positive_time_period_milliseconds,

    # Automation triggers
    cv.Optional(CONF_ON_SLEEP): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SleepTrigger),
    }),
    cv.Optional(CONF_ON_WAKE): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(WakeTrigger),
    }),
    cv.Optional(CONF_ON_SWIPE_LEFT): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SwipeLeftTrigger),
    }),
    cv.Optional(CONF_ON_SWIPE_RIGHT): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SwipeRightTrigger),
    }),
    cv.Optional(CONF_ON_SWIPE_UP): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SwipeUpTrigger),
    }),
    cv.Optional(CONF_ON_SWIPE_DOWN): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SwipeDownTrigger),
    }),
    cv.Optional(CONF_ON_PAGE_SHOW): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PageShowTrigger),
    }),
    cv.Optional(CONF_ON_PAGE_HIDE): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PageHideTrigger),
    }),

    # Optional SD card — ESP-IDF only.
    # When present, SentIO mounts the card during setup() and registers the
    # LVGL 'S:' driver.  Bare paths (no /sdcard or /littlefs prefix) resolve
    # to SD first; LittleFS is used as fallback when SD is absent.
    cv.Optional(CONF_SD_CARD): _sd_card_schema,
}).extend(cv.COMPONENT_SCHEMA)


@automation.register_condition(
    "sentio.is_page",
    IsPageCondition,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(SentioComponent),
            cv.Required(CONF_PAGE_ID): cv.string,
        },
        key=CONF_PAGE_ID,
    ),
)
async def sentio_is_page_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_page_id(config[CONF_PAGE_ID]))
    return var


# ---------------------------------------------------------------------------
# Code generator
# ---------------------------------------------------------------------------
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Ensure the SentIO header is visible to all generated lambdas
    # (including on_boot: lambdas in the esphome: block that reference
    #  esphome::sentio::SentioComponent::instance).
    cg.add_global(cg.RawStatement('#include "esphome/components/sentio/sentio.h"'))

    # Touch source (optional smart-touch proxy)
    if CONF_TOUCH_SOURCE in config:
        ts = await cg.get_variable(config[CONF_TOUCH_SOURCE])
        cg.add(var.set_touch_source(ts))

        # Auto-detect reset pin from touchscreen configuration
        if CONF_SOFT_SLEEP_ONLY in config:
            cg.add(var.set_soft_sleep_only(config[CONF_SOFT_SLEEP_ONLY]))
        else:
            from esphome import core
            has_reset_pin = False
            for ts_cfg in core.CORE.config.get("touchscreen", []):
                if ts_cfg.get("id") == config[CONF_TOUCH_SOURCE]:
                    if "reset_pin" in ts_cfg:
                        has_reset_pin = True
                    break
            cg.add(var.set_soft_sleep_only(not has_reset_pin))
    else:
        cg.add(var.set_soft_sleep_only(True))

    # Backlight — mutually exclusive options
    if CONF_BACKLIGHT_LIGHT in config:
        bl = await cg.get_variable(config[CONF_BACKLIGHT_LIGHT])
        cg.add(var.set_backlight_light(bl))

    if CONF_BACKLIGHT_OUTPUT in config:
        bl = await cg.get_variable(config[CONF_BACKLIGHT_OUTPUT])
        cg.add(var.set_backlight_output(bl))

    # Power management flags
    cg.add(var.set_sleep_timeout(config[CONF_SLEEP_TIMEOUT]))
    cg.add(var.set_suppress_wake_click(config[CONF_SUPPRESS_WAKE_CLICK]))
    cg.add(var.set_anti_burn_in(config[CONF_ANTI_BURN_IN]))
    cg.add(var.set_display_off_before_sleep(config[CONF_DISPLAY_OFF_BEFORE_SLEEP]))

    # Startup layout file
    if config[CONF_STARTUP_LAYOUT]:
        cg.add(var.set_startup_layout(config[CONF_STARTUP_LAYOUT]))

    # Gesture & Input tuning
    cg.add(var.set_gesture_threshold(config[CONF_GESTURE_THRESHOLD]))
    cg.add(var.set_gesture_timeout(config[CONF_GESTURE_TIMEOUT]))
    cg.add(var.set_long_press_time(config[CONF_LONG_PRESS_TIME]))

    # SD card (ESP32 / ESP-IDF only)
    if CONF_SD_CARD in config:
        if not CORE.is_esp32:
            raise cv.Invalid(
                "sentio sentio_sd requires an ESP32 target with 'framework: type: esp-idf'. "
                "Other platforms are not supported for SD card access."
            )
        sd = config[CONF_SD_CARD]
        cg.add_define("USE_SENTIO_SD")
        # Declare ESP-IDF component dependencies for SD card / FatFS
        cg.add_library("fatfs", None)
        cg.add_library("sdmmc", None)
        cg.add_library("vfs", None)
        cg.add_library("driver", None)
        cg.add_library("spi_flash", None)
        # FatFS Long Filename (LFN) support — required for filenames > 8.3 chars.
        add_idf_sdkconfig_option("CONFIG_FATFS_LFN_HEAP",     "y")
        add_idf_sdkconfig_option("CONFIG_FATFS_CODEPAGE_437", "y")
        # Allow up to 5 simultaneous open file handles (LVGL + services).
        add_idf_sdkconfig_option("CONFIG_FATFS_FS_LOCK",      "5")

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
        if CONF_SD_SPI_HOST in sd:
            cg.add(var.set_sd_spi_host(sd[CONF_SD_SPI_HOST]))
        cg.add(var.set_sd_cs_hardwired(sd[CONF_SD_CS_HARDWIRED]))
        cg.add(var.set_sd_format_if_mount_failed(sd[CONF_SD_FORMAT_IF_MOUNT_FAILED]))

    # Register all local sensors so they can be bound by string ID at runtime
    from esphome import core
    if "sensor" in core.CORE.config:
        cg.add_define("USE_SENSOR")
        for sensor_cfg in core.CORE.config.get("sensor", []):
            if "id" in sensor_cfg:
                sensor_id = sensor_cfg["id"]
                sens_var = await cg.get_variable(sensor_id)
                cg.add(var.register_local_sensor(sensor_id.id, sens_var))

    # Build automations — Trigger constructor self-registers via callback manager.
    for conf in config.get(CONF_ON_SLEEP, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_WAKE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SWIPE_LEFT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SWIPE_RIGHT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SWIPE_UP, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SWIPE_DOWN, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PAGE_SHOW, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)

    for conf in config.get(CONF_ON_PAGE_HIDE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)
