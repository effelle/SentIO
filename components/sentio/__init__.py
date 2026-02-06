import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import touchscreen
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
    cv.Optional(CONF_ON_SWIPE_LEFT): automation.validate_automation(single=True),
    cv.Optional(CONF_ON_SWIPE_RIGHT): automation.validate_automation(single=True),
    cv.Optional(CONF_ON_TAP): automation.validate_automation(single=True),
    cv.Optional(CONF_ON_WAKE): automation.validate_automation(single=True),
    cv.Optional(CONF_ON_SLEEP): automation.validate_automation(single=True),
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
