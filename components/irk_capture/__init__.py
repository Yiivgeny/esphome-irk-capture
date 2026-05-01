import esphome.codegen as cg
from esphome import automation
from esphome.components import esp32_ble, esp32_ble_server, switch
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID, ENTITY_CATEGORY_CONFIG

AUTO_LOAD = ["esp32_ble", "esp32_ble_server", "switch"]
DEPENDENCIES = ["esp32"]
CODEOWNERS = ["@evgeny"]

CONF_AUTO_DISCONNECT = "auto_disconnect"
CONF_BLE_ID = "ble_id"
CONF_BLE_NAME = "ble_name"
CONF_BLE_SERVER_ID = "ble_server_id"
CONF_ENROLL_SWITCH = "enroll_switch"
CONF_ON_IRK = "on_irk"
CONF_VISIBLE_SWITCH = "visible_switch"

irk_capture_ns = cg.esphome_ns.namespace("irk_capture")
IrkCapture = irk_capture_ns.class_(
    "IrkCapture",
    cg.Component,
    cg.Parented.template(esp32_ble_server.BLEServer),
)
IrkCaptureEnrollSwitch = irk_capture_ns.class_(
    "IrkCaptureEnrollSwitch", switch.Switch
)
IrkCaptureVisibleSwitch = irk_capture_ns.class_(
    "IrkCaptureVisibleSwitch", switch.Switch
)
IrkFoundTrigger = irk_capture_ns.class_(
    "IrkFoundTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IrkCapture),
        cv.GenerateID(CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.GenerateID(CONF_BLE_SERVER_ID): cv.use_id(esp32_ble_server.BLEServer),
        cv.Optional(CONF_AUTO_DISCONNECT, default=True): cv.boolean,
        cv.Optional(CONF_BLE_NAME): cv.All(cv.string, cv.Length(max=20)),
        cv.Optional(CONF_ENROLL_SWITCH): switch.switch_schema(
            IrkCaptureEnrollSwitch,
            icon="mdi:bluetooth-connect",
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="RESTORE_DEFAULT_OFF",
        ),
        cv.Optional(CONF_VISIBLE_SWITCH): switch.switch_schema(
            IrkCaptureVisibleSwitch,
            icon="mdi:bluetooth-settings",
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="RESTORE_DEFAULT_OFF",
        ),
        cv.Optional(CONF_ON_IRK): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(IrkFoundTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_BLE_SERVER_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_auto_disconnect(config[CONF_AUTO_DISCONNECT]))

    ble_parent = await cg.get_variable(config[CONF_BLE_ID])
    if (name := config.get(CONF_BLE_NAME)) is not None:
        cg.add(ble_parent.set_name(name))
    esp32_ble.register_gap_event_handler(ble_parent, var)
    esp32_ble.register_gatts_event_handler(ble_parent, var)

    if CONF_ENROLL_SWITCH in config:
        sw = await switch.new_switch(config[CONF_ENROLL_SWITCH], var)
        cg.add(var.set_enroll_switch(sw))

    if CONF_VISIBLE_SWITCH in config:
        sw = await switch.new_switch(config[CONF_VISIBLE_SWITCH], var)
        cg.add(var.set_visible_switch(sw))

    for conf in config.get(CONF_ON_IRK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_irk_trigger(trigger))
        await automation.build_automation(
            trigger,
            [(cg.std_string, "irk"), (cg.std_string, "address")],
            conf,
        )
