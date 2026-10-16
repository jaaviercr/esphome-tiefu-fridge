import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    ble_client,
    climate,
    event,
    select,
    sensor,
    text_sensor,
)
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_VOLT,
)
from esphome.types import ConfigType

AUTO_LOAD = ["binary_sensor", "climate", "event", "select", "sensor", "text_sensor"]
DEPENDENCIES = ["ble_client"]
DOMAIN = "tiefu_fridge"

# YAML options and defaults that users can override.
DEFAULT_UPDATE_INTERVAL = "30s"
CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_LEFT_TEMPERATURE = "left_temperature"
CONF_RIGHT_TEMPERATURE = "right_temperature"
CONF_RIGHT_TARGET_TEMPERATURE = "right_target_temperature"
CONF_VOLTAGE = "voltage"
CONF_BATTERY_PERCENTAGE = "battery_percentage"
CONF_LAST_FAULT = "last_fault"
CONF_FAULT_EVENT = "fault_event"
CONF_POWER = "power"
CONF_BATTERY_PROTECTION = "battery_protection"
CONF_COMPRESSOR_MODE = "compressor_mode"
CONF_PROTOCOL_STATE = "protocol_state"
CONF_CLIMATE = "climate"
CONF_BATTERY_PROTECTION_CONTROL = "battery_protection_control"
CONF_COMPRESSOR_MODE_CONTROL = "compressor_mode_control"
CONF_TEMPERATURE_UNIT_CONTROL = "temperature_unit_control"
BATTERY_PROTECTION_OPTIONS = ["low", "middle", "high"]
COMPRESSOR_MODE_OPTIONS = ["eco", "max"]

tiefu_fridge_ns = cg.esphome_ns.namespace("tiefu_fridge")
TiefuFridge = tiefu_fridge_ns.class_(
    "TiefuFridge", cg.PollingComponent, ble_client.BLEClientNode
)
TiefuFridgeClimate = tiefu_fridge_ns.class_("TiefuFridgeClimate", climate.Climate)
TiefuFridgeBatteryProtectionSelect = tiefu_fridge_ns.class_(
    "TiefuFridgeBatteryProtectionSelect", select.Select
)
TiefuFridgeCompressorModeSelect = tiefu_fridge_ns.class_(
    "TiefuFridgeCompressorModeSelect", select.Select
)
TiefuFridgeTemperatureUnitSelect = tiefu_fridge_ns.class_(
    "TiefuFridgeTemperatureUnitSelect", select.Select
)

TEMPERATURE_SENSOR_SCHEMA = sensor.sensor_schema(
    icon="mdi:thermometer",
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
    unit_of_measurement=UNIT_CELSIUS,
)
TARGET_TEMPERATURE_SENSOR_SCHEMA = sensor.sensor_schema(
    icon="mdi:thermometer-chevron-up",
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
    unit_of_measurement=UNIT_CELSIUS,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TiefuFridge),
        cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
        cv.Optional(CONF_LEFT_TEMPERATURE): TEMPERATURE_SENSOR_SCHEMA,
        cv.Optional(CONF_RIGHT_TEMPERATURE): TEMPERATURE_SENSOR_SCHEMA,
        cv.Optional(CONF_RIGHT_TARGET_TEMPERATURE): TARGET_TEMPERATURE_SENSOR_SCHEMA,
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            icon="mdi:car-battery",
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_VOLT,
        ),
        cv.Optional(CONF_BATTERY_PERCENTAGE): sensor.sensor_schema(
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            unit_of_measurement=UNIT_PERCENT,
        ),
        cv.Optional(CONF_LAST_FAULT): text_sensor.text_sensor_schema(icon="mdi:alert-circle"),
        cv.Optional(CONF_FAULT_EVENT): event.event_schema(icon="mdi:alert-circle"),
        cv.Optional(CONF_POWER): binary_sensor.binary_sensor_schema(icon="mdi:power"),
        cv.Optional(CONF_BATTERY_PROTECTION): text_sensor.text_sensor_schema(icon="mdi:car-battery"),
        cv.Optional(CONF_COMPRESSOR_MODE): text_sensor.text_sensor_schema(icon="mdi:speedometer"),
        cv.Optional(CONF_PROTOCOL_STATE): text_sensor.text_sensor_schema(
            icon="mdi:code-tags", entity_category="diagnostic"
        ).extend({cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True): cv.boolean}),
        cv.Optional(CONF_CLIMATE): climate.climate_schema(TiefuFridgeClimate, icon="mdi:fridge"),
        cv.Optional(CONF_BATTERY_PROTECTION_CONTROL): select.select_schema(
            TiefuFridgeBatteryProtectionSelect, icon="mdi:car-battery"
        ),
        cv.Optional(CONF_COMPRESSOR_MODE_CONTROL): select.select_schema(
            TiefuFridgeCompressorModeSelect, icon="mdi:speedometer"
        ),
        cv.Optional(CONF_TEMPERATURE_UNIT_CONTROL): select.select_schema(
            TiefuFridgeTemperatureUnitSelect, icon="mdi:thermometer"
        ),
    }
).extend(cv.COMPONENT_SCHEMA).extend(cv.polling_component_schema(DEFAULT_UPDATE_INTERVAL))


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    sensor_setters = {
        CONF_LEFT_TEMPERATURE: "set_left_temperature_sensor",
        CONF_RIGHT_TEMPERATURE: "set_right_temperature_sensor",
        CONF_RIGHT_TARGET_TEMPERATURE: "set_right_target_temperature_sensor",
        CONF_VOLTAGE: "set_voltage_sensor",
        CONF_BATTERY_PERCENTAGE: "set_battery_percentage_sensor",
    }
    for key, setter in sensor_setters.items():
        if key in config:
            entity = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(entity))

    if CONF_POWER in config:
        entity = await binary_sensor.new_binary_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(entity))

    text_sensor_setters = {
        CONF_LAST_FAULT: "set_last_fault_sensor",
        CONF_BATTERY_PROTECTION: "set_battery_protection_sensor",
        CONF_COMPRESSOR_MODE: "set_compressor_mode_sensor",
        CONF_PROTOCOL_STATE: "set_protocol_state_sensor",
    }
    for key, setter in text_sensor_setters.items():
        if key in config:
            entity = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(entity))

    if CONF_CLIMATE in config:
        climate_entity = await climate.new_climate(config[CONF_CLIMATE], var)
        cg.add(var.set_climate(climate_entity))

    if CONF_BATTERY_PROTECTION_CONTROL in config:
        battery_select = await select.new_select(
            config[CONF_BATTERY_PROTECTION_CONTROL],
            var,
            options=BATTERY_PROTECTION_OPTIONS,
        )
        cg.add(var.set_battery_protection_control(battery_select))

    if CONF_COMPRESSOR_MODE_CONTROL in config:
        mode_select = await select.new_select(
            config[CONF_COMPRESSOR_MODE_CONTROL],
            var,
            options=COMPRESSOR_MODE_OPTIONS,
        )
        cg.add(var.set_compressor_mode_control(mode_select))

    if CONF_TEMPERATURE_UNIT_CONTROL in config:
        unit_select = await select.new_select(
            config[CONF_TEMPERATURE_UNIT_CONTROL],
            var,
            options=["celsius", "fahrenheit"],
        )
        cg.add(var.set_temperature_unit_control(unit_select))

    if CONF_FAULT_EVENT in config:
        fault_event = await event.new_event(
            config[CONF_FAULT_EVENT],
            event_types=["e1", "e2", "e3", "e4", "e5", "e6"],
        )
        cg.add(var.set_fault_event(fault_event))