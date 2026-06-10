"""
SentIO — ESPHome Dynamic LVGL Overlay Component
Phase 1: Python configuration schema and C++ code generator.

Registers:
  - Component class SentioComponent
  - YAML options: touch_source, backlight, sleep, burn-in, etc.
  - on_sleep / on_wake automation triggers
  - Four HA API services: run_jsonl, clear, load_layout, save_layout_line
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import light, output, touchscreen
from esphome.const import CONF_ID, CONF_TRIGGER_ID

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
