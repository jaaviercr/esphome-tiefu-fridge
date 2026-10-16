#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/event/event.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"

#include <esp_gattc_api.h>
#include <array>
#include <limits>

namespace esphome {
namespace tiefu_fridge {

class TiefuFridge;

class TiefuFridgeClimate : public climate::Climate {
 public:
  explicit TiefuFridgeClimate(TiefuFridge *parent) : parent_(parent) {}
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  TiefuFridge *parent_;
};

class TiefuFridgeBatteryProtectionSelect : public select::Select {
 public:
  explicit TiefuFridgeBatteryProtectionSelect(TiefuFridge *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;

  TiefuFridge *parent_;
};

class TiefuFridgeCompressorModeSelect : public select::Select {
 public:
  explicit TiefuFridgeCompressorModeSelect(TiefuFridge *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;

  TiefuFridge *parent_;
};

class TiefuFridgeTemperatureUnitSelect : public select::Select {
 public:
  explicit TiefuFridgeTemperatureUnitSelect(TiefuFridge *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;

  TiefuFridge *parent_;
};

class TiefuFridge : public PollingComponent, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void update() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  bool control_power(bool enabled);
  void control_target_temperature(float temperature);
  void control_climate(const climate::ClimateCall &call);
  void control_battery_protection(size_t index);
  void control_compressor_mode(size_t index);
  void control_temperature_unit(size_t index);

  void set_left_temperature_sensor(sensor::Sensor *sensor) {
    this->left_temperature_sensor_ = sensor;
  }
  void set_right_temperature_sensor(sensor::Sensor *sensor) {
    this->right_temperature_sensor_ = sensor;
  }
  void set_left_target_temperature_sensor(sensor::Sensor *sensor) {
    this->left_target_temperature_sensor_ = sensor;
  }
  void set_right_target_temperature_sensor(sensor::Sensor *sensor) {
    this->right_target_temperature_sensor_ = sensor;
  }
  void set_voltage_sensor(sensor::Sensor *sensor) {
    this->voltage_sensor_ = sensor;
  }
  void set_battery_percentage_sensor(sensor::Sensor *sensor) { this->battery_percentage_sensor_ = sensor; }
  void set_last_fault_sensor(text_sensor::TextSensor *sensor) {
    this->last_fault_sensor_ = sensor;
  }
  void set_fault_event(event::Event *fault_event) {
    this->fault_event_ = fault_event;
  }
  void set_power_sensor(binary_sensor::BinarySensor *sensor) {
    this->power_sensor_ = sensor;
  }
  void set_climate(TiefuFridgeClimate *climate_entity) {
    this->climate_ = climate_entity;
  }
  void set_battery_protection_control(TiefuFridgeBatteryProtectionSelect *control) {
    this->battery_protection_control_ = control;
  }
  void set_compressor_mode_control(TiefuFridgeCompressorModeSelect *control) {
    this->compressor_mode_control_ = control;
  }
  void set_temperature_unit_control(TiefuFridgeTemperatureUnitSelect *control) {
    this->temperature_unit_control_ = control;
  }
  void set_battery_protection_sensor(text_sensor::TextSensor *sensor) {
    this->battery_protection_sensor_ = sensor;
  }
  void set_compressor_mode_sensor(text_sensor::TextSensor *sensor) {
    this->compressor_mode_sensor_ = sensor;
  }
  void set_protocol_state_sensor(text_sensor::TextSensor *sensor) {
    this->protocol_state_sensor_ = sensor;
  }

 protected:
  static constexpr uint32_t MQTT_STATE_CLEAR_RETRY_INTERVAL_ID = 0;
  static constexpr size_t MAX_RX_BUFFER_SIZE = 256;

  void clear_entity_states_();
#ifdef USE_MQTT
  bool clear_mqtt_entity_states_();
#endif
  void schedule_state_timeout_();
  void request_state_();
  bool send_command_(StringRef command);
  void handle_frame_(StringRef frame);

  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};
  std::array<char, MAX_RX_BUFFER_SIZE + 1> rx_buffer_{};
  size_t rx_buffer_size_{0};
  int power_state_{-1};
  bool entity_states_cleared_{false};
  float pending_target_temperature_{std::numeric_limits<float>::quiet_NaN()};

  TiefuFridgeClimate *climate_{nullptr};
  sensor::Sensor *left_temperature_sensor_{nullptr};
  sensor::Sensor *right_temperature_sensor_{nullptr};
  sensor::Sensor *left_target_temperature_sensor_{nullptr};
  sensor::Sensor *right_target_temperature_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *battery_percentage_sensor_{nullptr};
  event::Event *fault_event_{nullptr};
  binary_sensor::BinarySensor *power_sensor_{nullptr};
  TiefuFridgeBatteryProtectionSelect *battery_protection_control_{nullptr};
  TiefuFridgeCompressorModeSelect *compressor_mode_control_{nullptr};
  TiefuFridgeTemperatureUnitSelect *temperature_unit_control_{nullptr};
  text_sensor::TextSensor *last_fault_sensor_{nullptr};
  text_sensor::TextSensor *battery_protection_sensor_{nullptr};
  text_sensor::TextSensor *compressor_mode_sensor_{nullptr};
  text_sensor::TextSensor *protocol_state_sensor_{nullptr};
};

}  // namespace tiefu_fridge
}  // namespace esphome