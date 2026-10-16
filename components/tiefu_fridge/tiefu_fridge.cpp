#include "tiefu_fridge.h"

#include "esphome/core/controller_registry.h"
#include "esphome/core/log.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/mqtt/mqtt_component.h"
#endif

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace tiefu_fridge {

namespace espbt = esphome::esp32_ble_tracker;

static const char *const TAG = "tiefu_fridge";

namespace {

// Fixed fridge protocol and device limits.
constexpr uint16_t FRIDGE_SERVICE_UUID = 0xFFF0;
constexpr uint16_t FRIDGE_WRITE_CHARACTERISTIC_UUID = 0xFFF1;
constexpr uint16_t FRIDGE_NOTIFY_CHARACTERISTIC_UUID = 0xFFF4;
constexpr float MIN_TARGET_TEMPERATURE_C = -22.0f;
constexpr float MAX_TARGET_TEMPERATURE_C = 10.0f;
constexpr float TARGET_TEMPERATURE_STEP_C = 1.0f;
constexpr size_t SINGLE_ZONE_STATE_FIELD_COUNT = 10;
constexpr size_t DUAL_ZONE_STATE_FIELD_COUNT = 13;
constexpr size_t MAX_STATE_FIELD_COUNT = 14;
constexpr long MIN_PROTOCOL_FIELD_VALUE = -32768;
constexpr long MAX_PROTOCOL_FIELD_VALUE = 32767;
constexpr long MIN_FAULT_CODE = 1;
constexpr long MAX_FAULT_CODE = 6;
constexpr float FAHRENHEIT_OFFSET = 32.0f;
constexpr float CELSIUS_CONVERSION_NUMERATOR = 5.0f;
constexpr float CELSIUS_CONVERSION_DENOMINATOR = 9.0f;
constexpr float RAW_VOLTAGE_DIVISOR = 10.0f;
constexpr int BATTERY_0_PERCENT_MAX_RAW_VOLTAGE = 96;
constexpr int BATTERY_20_PERCENT_MAX_RAW_VOLTAGE = 103;
constexpr int BATTERY_40_PERCENT_MAX_RAW_VOLTAGE = 111;
constexpr int BATTERY_60_PERCENT_MAX_RAW_VOLTAGE = 117;
constexpr int BATTERY_80_PERCENT_MAX_RAW_VOLTAGE = 124;
constexpr int BATTERY_PERCENTAGE_STEP = 20;

// Internal parser and update timing; these are not YAML options.
constexpr uint32_t STATE_TIMEOUT_UPDATE_INTERVAL_MULTIPLIER = 3;
constexpr uint32_t CONTROL_REFRESH_DELAY_MS = 500;
constexpr uint32_t MQTT_STATE_CLEAR_RETRY_INTERVAL_MS = 5000;
constexpr size_t TARGET_COMMAND_BUFFER_SIZE = 16;
constexpr size_t BATTERY_PROTECTION_COMMAND_VALUE_OFFSET = 9;
constexpr size_t MAX_FAULT_DESCRIPTION_LENGTH = 128;
constexpr char BATTERY_PROTECTION_LOW[] = "low";
constexpr char BATTERY_PROTECTION_MIDDLE[] = "middle";
constexpr char BATTERY_PROTECTION_HIGH[] = "high";
constexpr size_t MAX_BATTERY_PROTECTION_STATE_LENGTH = sizeof(BATTERY_PROTECTION_MIDDLE) - 1;
constexpr char COMPRESSOR_MODE_ECO[] = "eco";
constexpr char COMPRESSOR_MODE_MAX[] = "max";
constexpr size_t MAX_COMPRESSOR_MODE_STATE_LENGTH =
  sizeof(COMPRESSOR_MODE_ECO) >= sizeof(COMPRESSOR_MODE_MAX) ? sizeof(COMPRESSOR_MODE_ECO) - 1
                                 : sizeof(COMPRESSOR_MODE_MAX) - 1;
constexpr char MQTT_UNKNOWN_STATE[] = "None";
constexpr size_t MQTT_UNKNOWN_STATE_LENGTH = sizeof(MQTT_UNKNOWN_STATE) - 1;

enum StateField : size_t {
  STATE_FIELD_MODEL,
  STATE_FIELD_POWER,
  STATE_FIELD_TEMPERATURE_UNIT,
  STATE_FIELD_LEFT_TEMPERATURE,
  STATE_FIELD_RIGHT_TEMPERATURE,
  STATE_FIELD_LEFT_TARGET_TEMPERATURE,
  STATE_FIELD_RIGHT_TARGET_TEMPERATURE,
  STATE_FIELD_BATTERY_PROTECTION,
  STATE_FIELD_RAW_VOLTAGE,
  STATE_FIELD_COMPRESSOR_MODE,
};

bool starts_with(StringRef value, const char *prefix, size_t prefix_length) {
  return value.size() >= prefix_length && std::memcmp(value.c_str(), prefix, prefix_length) == 0;
}

}  // namespace

climate::ClimateTraits TiefuFridgeClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_COOL});
  traits.set_visual_min_temperature(MIN_TARGET_TEMPERATURE_C);
  traits.set_visual_max_temperature(MAX_TARGET_TEMPERATURE_C);
  traits.set_visual_temperature_step(TARGET_TEMPERATURE_STEP_C);
  return traits;
}

void TiefuFridgeClimate::control(const climate::ClimateCall &call) { this->parent_->control_climate(call); }

void TiefuFridgeBatteryProtectionSelect::control(size_t index) {
  this->parent_->control_battery_protection(index);
}

void TiefuFridgeCompressorModeSelect::control(size_t index) { this->parent_->control_compressor_mode(index); }

void TiefuFridgeTemperatureUnitSelect::control(size_t index) {
  this->parent_->control_temperature_unit(index);
}

void TiefuFridge::setup() {
  if (this->last_fault_sensor_ != nullptr) {
    this->last_fault_sensor_->state.reserve(MAX_FAULT_DESCRIPTION_LENGTH);
  }
  if (this->battery_protection_sensor_ != nullptr) {
    this->battery_protection_sensor_->state.reserve(MAX_BATTERY_PROTECTION_STATE_LENGTH);
  }
  if (this->compressor_mode_sensor_ != nullptr) {
    this->compressor_mode_sensor_->state.reserve(MAX_COMPRESSOR_MODE_STATE_LENGTH);
  }
  if (this->protocol_state_sensor_ != nullptr) {
    this->protocol_state_sensor_->state.reserve(MAX_RX_BUFFER_SIZE);
  }
  this->clear_entity_states_();
}

void TiefuFridge::update() { this->request_state_(); }

void TiefuFridge::schedule_state_timeout_() {
  this->entity_states_cleared_ = false;
  this->set_timeout("fridge_state_timeout", this->get_update_interval() * STATE_TIMEOUT_UPDATE_INTERVAL_MULTIPLIER,
                    [this]() { this->clear_entity_states_(); });
}

void TiefuFridge::clear_entity_states_() {
  if (this->entity_states_cleared_) {
    return;
  }
  this->entity_states_cleared_ = true;
  this->power_state_ = -1;
  this->pending_target_temperature_ = std::numeric_limits<float>::quiet_NaN();

  if (this->climate_ != nullptr) {
    this->climate_->current_temperature = std::numeric_limits<float>::quiet_NaN();
    this->climate_->target_temperature = std::numeric_limits<float>::quiet_NaN();
    this->climate_->mode = climate::CLIMATE_MODE_OFF;
    this->climate_->publish_state();
  }
  if (this->left_temperature_sensor_ != nullptr) {
    this->left_temperature_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->left_temperature_sensor_);
#endif
  }
  if (this->right_temperature_sensor_ != nullptr) {
    this->right_temperature_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->right_temperature_sensor_);
#endif
  }
  if (this->left_target_temperature_sensor_ != nullptr) {
    this->left_target_temperature_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->left_target_temperature_sensor_);
#endif
  }
  if (this->right_target_temperature_sensor_ != nullptr) {
    this->right_target_temperature_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->right_target_temperature_sensor_);
#endif
  }
  if (this->voltage_sensor_ != nullptr) {
    this->voltage_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->voltage_sensor_);
#endif
  }
  if (this->battery_percentage_sensor_ != nullptr) {
    this->battery_percentage_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_sensor_update(this->battery_percentage_sensor_);
#endif
  }
  if (this->power_sensor_ != nullptr) {
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_BINARY_SENSOR)
    const bool had_state = this->power_sensor_->has_state();
#endif
    this->power_sensor_->invalidate_state();
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_BINARY_SENSOR)
    if (!had_state) {
      ControllerRegistry::notify_binary_sensor_update(this->power_sensor_);
    }
#endif
  }
  if (this->last_fault_sensor_ != nullptr) {
    this->last_fault_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_text_sensor_update(this->last_fault_sensor_);
#endif
  }
  if (this->battery_protection_sensor_ != nullptr) {
    this->battery_protection_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_text_sensor_update(this->battery_protection_sensor_);
#endif
  }
  if (this->compressor_mode_sensor_ != nullptr) {
    this->compressor_mode_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_text_sensor_update(this->compressor_mode_sensor_);
#endif
  }
  if (this->protocol_state_sensor_ != nullptr) {
    this->protocol_state_sensor_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_text_sensor_update(this->protocol_state_sensor_);
#endif
  }
  if (this->battery_protection_control_ != nullptr) {
    this->battery_protection_control_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_select_update(this->battery_protection_control_);
#endif
  }
  if (this->compressor_mode_control_ != nullptr) {
    this->compressor_mode_control_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_select_update(this->compressor_mode_control_);
#endif
  }
  if (this->temperature_unit_control_ != nullptr) {
    this->temperature_unit_control_->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_select_update(this->temperature_unit_control_);
#endif
  }
#ifdef USE_MQTT
  if (!this->clear_mqtt_entity_states_()) {
    this->set_interval(MQTT_STATE_CLEAR_RETRY_INTERVAL_ID, MQTT_STATE_CLEAR_RETRY_INTERVAL_MS, [this]() {
      if (this->clear_mqtt_entity_states_()) {
        this->cancel_interval(MQTT_STATE_CLEAR_RETRY_INTERVAL_ID);
        if (this->node_state == espbt::ClientState::ESTABLISHED) {
          this->request_state_();
        }
      }
    });
  }
#endif
}

#ifdef USE_MQTT
bool TiefuFridge::clear_mqtt_entity_states_() {
  auto *mqtt_client = mqtt::global_mqtt_client;
  if (mqtt_client == nullptr || !mqtt_client->is_connected()) {
    return false;
  }

  const auto &topic_prefix = mqtt_client->get_topic_prefix();
  if (topic_prefix.empty()) {
    return true;
  }

  bool success = true;
  auto clear_topic = [&](const char *component_type, EntityBase *entity, const char *suffix, const char *payload,
                         size_t payload_length) {
    if (entity == nullptr) {
      return;
    }

    char object_id[OBJECT_ID_MAX_LEN];
    const size_t object_id_length = entity->write_object_id_to(object_id, sizeof(object_id));
    if (object_id_length == 0 || object_id_length >= sizeof(object_id)) {
      success = false;
      return;
    }

    char topic[mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN];
    const int topic_length = std::snprintf(topic, sizeof(topic), "%s/%s/%.*s/%s", topic_prefix.c_str(),
                                           component_type, static_cast<int>(object_id_length), object_id, suffix);
    if (topic_length < 0 || static_cast<size_t>(topic_length) >= sizeof(topic) ||
        !mqtt_client->publish(topic, payload, payload_length, 0, true)) {
      success = false;
    }
  };
  auto clear_text_state = [&](const char *component_type, EntityBase *entity) {
    clear_topic(component_type, entity, "state", "", 0);
  };
  auto clear_numeric_state = [&](EntityBase *entity) {
    clear_topic("sensor", entity, "state", MQTT_UNKNOWN_STATE, MQTT_UNKNOWN_STATE_LENGTH);
  };
  auto clear_select_state = [&](EntityBase *entity) {
    clear_topic("select", entity, "state", MQTT_UNKNOWN_STATE, MQTT_UNKNOWN_STATE_LENGTH);
  };

  clear_numeric_state(this->left_temperature_sensor_);
  clear_numeric_state(this->right_temperature_sensor_);
  clear_numeric_state(this->left_target_temperature_sensor_);
  clear_numeric_state(this->right_target_temperature_sensor_);
  clear_numeric_state(this->voltage_sensor_);
  clear_numeric_state(this->battery_percentage_sensor_);
  clear_text_state("sensor", this->last_fault_sensor_);
  clear_text_state("sensor", this->battery_protection_sensor_);
  clear_text_state("sensor", this->compressor_mode_sensor_);
  clear_text_state("sensor", this->protocol_state_sensor_);
  clear_text_state("binary_sensor", this->power_sensor_);
  clear_select_state(this->battery_protection_control_);
  clear_select_state(this->compressor_mode_control_);
  clear_select_state(this->temperature_unit_control_);
  clear_topic("climate", this->climate_, "current_temperature/state", MQTT_UNKNOWN_STATE, MQTT_UNKNOWN_STATE_LENGTH);
  clear_topic("climate", this->climate_, "target_temperature/state", MQTT_UNKNOWN_STATE, MQTT_UNKNOWN_STATE_LENGTH);
  return success;
}
#endif

void TiefuFridge::request_state_() {
  this->send_command_(StringRef::from_lit("/S00/1/-01\n"));
}

bool TiefuFridge::send_command_(StringRef command) {
  if (this->node_state != espbt::ClientState::ESTABLISHED || this->write_handle_ == 0) {
    ESP_LOGD(TAG, "BLE link not ready; skipping command");
    return false;
  }

  esp_err_t err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                           this->write_handle_, command.size(),
                                           const_cast<uint8_t *>(command.byte()),
                                           ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "BLE command write failed: %d", err);
    return false;
  }
  return true;
}

bool TiefuFridge::control_power(bool enabled) {
  const char *command = enabled ? "/S01/1/001\n" : "/S01/1/000\n";
  if (!this->send_command_(StringRef(command))) {
    return false;
  }
  this->set_timeout("refresh_after_control", CONTROL_REFRESH_DELAY_MS, [this]() { this->request_state_(); });
  return true;
}

void TiefuFridge::control_climate(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    const auto mode = *call.get_mode();
    if (mode == climate::CLIMATE_MODE_OFF) {
      this->pending_target_temperature_ = std::numeric_limits<float>::quiet_NaN();
      this->control_power(false);
      return;
    }
    if (mode != climate::CLIMATE_MODE_COOL) {
      ESP_LOGW(TAG, "Unsupported climate mode requested");
      return;
    }
    if (this->power_state_ != 1) {
      if (call.get_target_temperature().has_value()) {
        this->pending_target_temperature_ = *call.get_target_temperature();
      }
      if (!this->control_power(true)) {
        this->pending_target_temperature_ = std::numeric_limits<float>::quiet_NaN();
      }
      return;
    }
  }

  if (call.get_target_temperature().has_value()) {
    this->control_target_temperature(*call.get_target_temperature());
  }
}

void TiefuFridge::control_target_temperature(float temperature) {
  if (this->power_state_ != 1) {
    ESP_LOGW(TAG, "Ignoring target temperature change while fridge is off or state is unknown");
    return;
  }

  const int value = static_cast<int>(std::lround(temperature));
  if (value < MIN_TARGET_TEMPERATURE_C || value > MAX_TARGET_TEMPERATURE_C) {
    ESP_LOGW(TAG, "Target temperature out of range: %d", value);
    return;
  }

  char command[TARGET_COMMAND_BUFFER_SIZE];
  if (value >= 0) {
    std::snprintf(command, sizeof(command), "/S03/1/%03d\n", value);
  } else {
    std::snprintf(command, sizeof(command), "/S03/1/-%02d\n", -value);
  }
  if (this->send_command_(StringRef(command))) {
    this->set_timeout("refresh_after_control", CONTROL_REFRESH_DELAY_MS, [this]() { this->request_state_(); });
  }
}

void TiefuFridge::control_battery_protection(size_t index) {
  if (this->power_state_ != 1 || index >= 3) {
    ESP_LOGW(TAG, "Ignoring battery protection change while fridge is off or option is invalid");
    return;
  }

  char command[] = "/S04/1/00X\n";
  command[BATTERY_PROTECTION_COMMAND_VALUE_OFFSET] = '1' + index;
  if (this->send_command_(StringRef(command))) {
    this->set_timeout("refresh_after_control", CONTROL_REFRESH_DELAY_MS, [this]() { this->request_state_(); });
  }
}

void TiefuFridge::control_compressor_mode(size_t index) {
  if (this->power_state_ != 1 || index > 1) {
    ESP_LOGW(TAG, "Ignoring compressor mode change while fridge is off or option is invalid");
    return;
  }

  const char *command = index == 0 ? "/S06/1/001\n" : "/S06/1/002\n";
  if (this->send_command_(StringRef(command))) {
    this->set_timeout("refresh_after_control", CONTROL_REFRESH_DELAY_MS, [this]() { this->request_state_(); });
  }
}

void TiefuFridge::control_temperature_unit(size_t index) {
  if (this->power_state_ != 1 || index > 1) {
    ESP_LOGW(TAG, "Ignoring temperature unit change while fridge is off or option is invalid");
    return;
  }

  const char *command = index == 0 ? "/S07/1/001\n" : "/S07/1/002\n";
  if (this->send_command_(StringRef(command))) {
    this->set_timeout("refresh_after_control", CONTROL_REFRESH_DELAY_MS, [this]() { this->request_state_(); });
  }
}

void TiefuFridge::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t,
                                      esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *write_char = this->parent()->get_characteristic(FRIDGE_SERVICE_UUID, FRIDGE_WRITE_CHARACTERISTIC_UUID);
      auto *notify_char = this->parent()->get_characteristic(FRIDGE_SERVICE_UUID, FRIDGE_NOTIFY_CHARACTERISTIC_UUID);
      if (write_char == nullptr || notify_char == nullptr) {
        ESP_LOGW(TAG, "Fridge GATT characteristics FFF1/FFF4 not found");
        return;
      }

      this->write_handle_ = write_char->handle;
      this->notify_handle_ = notify_char->handle;
      this->rx_buffer_size_ = 0;
      esp_err_t err = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                        this->parent()->get_remote_bda(), this->notify_handle_);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not register FFF4 notifications: %d", err);
      }
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle == this->notify_handle_ && param->reg_for_notify.status == ESP_GATT_OK) {
        this->node_state = espbt::ClientState::ESTABLISHED;
        this->schedule_state_timeout_();
        this->request_state_();
      }
      break;
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_ || param->notify.value_len == 0) {
        break;
      }

      if (param->notify.value_len > MAX_RX_BUFFER_SIZE - this->rx_buffer_size_) {
        ESP_LOGW(TAG, "Discarding oversized incomplete BLE response");
        this->rx_buffer_size_ = 0;
        break;
      }
      std::memcpy(this->rx_buffer_.data() + this->rx_buffer_size_, param->notify.value,
                  param->notify.value_len);
      this->rx_buffer_size_ += param->notify.value_len;
      this->rx_buffer_[this->rx_buffer_size_] = '\0';

      size_t consumed = 0;
      while (consumed < this->rx_buffer_size_) {
        const char *newline_ptr = static_cast<const char *>(
            std::memchr(this->rx_buffer_.data() + consumed, '\n', this->rx_buffer_size_ - consumed));
        if (newline_ptr == nullptr) {
          break;
        }
        const size_t newline = static_cast<size_t>(newline_ptr - this->rx_buffer_.data());
        size_t frame_length = newline - consumed;
        if (frame_length > 0 && this->rx_buffer_[consumed + frame_length - 1] == '\r') {
          frame_length--;
        }
        this->rx_buffer_[consumed + frame_length] = '\0';
        this->handle_frame_(StringRef(this->rx_buffer_.data() + consumed, frame_length));
        consumed = newline + 1;
      }
      if (consumed > 0) {
        const size_t remaining = this->rx_buffer_size_ - consumed;
        if (remaining > 0) {
          std::memmove(this->rx_buffer_.data(), this->rx_buffer_.data() + consumed, remaining);
        }
        this->rx_buffer_size_ = remaining;
        this->rx_buffer_[remaining] = '\0';
      }
      break;
    }
    case ESP_GATTC_CLOSE_EVT:
      this->write_handle_ = 0;
      this->notify_handle_ = 0;
      this->rx_buffer_size_ = 0;
      this->cancel_timeout("fridge_state_timeout");
      this->clear_entity_states_();
      break;
    default:
      break;
  }
}

void TiefuFridge::handle_frame_(StringRef frame) {
  static constexpr char ERROR_PREFIX[] = "/SEE/";
  constexpr size_t ERROR_PREFIX_LENGTH = sizeof(ERROR_PREFIX) - 1;
  if (starts_with(frame, ERROR_PREFIX, ERROR_PREFIX_LENGTH)) {
    size_t code_start = frame.size();
    while (code_start > 0 && frame[code_start - 1] != '/') {
      code_start--;
    }
    if (code_start >= frame.size()) {
      ESP_LOGW(TAG, "Ignoring malformed fridge fault frame");
      return;
    }

    const char *frame_data = frame.c_str();
    char *end = nullptr;
    long code = std::strtol(frame_data + code_start, &end, 10);
    if (end == frame_data + code_start || *end != '\0' || code < MIN_FAULT_CODE) {
      ESP_LOGW(TAG, "Ignoring malformed fridge fault code");
      return;
    }
    if (code >= MAX_FAULT_CODE) {
      code = MAX_FAULT_CODE;
    }
    this->schedule_state_timeout_();

    const char *event_type;
    const char *description;
    switch (code) {
      case 1:
        event_type = "e1";
        description = "E1: Battery protection / low input voltage. Check supply voltage and protection level.";
        break;
      case 2:
        event_type = "e2";
        description = "E2: Cooling fan fault. Check the fan.";
        break;
      case 3:
        event_type = "e3";
        description = "E3: Compressor anomaly. Power off for 5 minutes before restarting.";
        break;
      case 4:
        event_type = "e4";
        description = "E4: Compressor anomaly. Power off for 5 minutes before restarting.";
        break;
      case 5:
        event_type = "e5";
        description = "E5: Controller overheating. Check ventilation and fan; power off for 30 minutes.";
        break;
      default:
        event_type = "e6";
        description = "E6: NTC temperature probe open or short circuit. Check the probe wiring.";
        break;
    }

    ESP_LOGW(TAG, "Fridge fault: %s", description);
    if (this->fault_event_ != nullptr) {
      this->fault_event_->trigger(event_type);
    }
    if (this->last_fault_sensor_ != nullptr) {
      this->last_fault_sensor_->publish_state(description);
    }
    return;
  }

  static constexpr char PREFIX[] = "/S00/H/";
  constexpr size_t PREFIX_LENGTH = sizeof(PREFIX) - 1;
  if (!starts_with(frame, PREFIX, PREFIX_LENGTH)) {
    ESP_LOGD(TAG, "Ignoring non-state BLE frame");
    return;
  }

  std::array<int, MAX_STATE_FIELD_COUNT> values{};
  size_t value_count = 0;
  const char *cursor = frame.c_str() + PREFIX_LENGTH;
  while (*cursor != '\0' && value_count < values.size()) {
    char *end = nullptr;
    long value = std::strtol(cursor, &end, 10);
    if (end == cursor || value < MIN_PROTOCOL_FIELD_VALUE || value > MAX_PROTOCOL_FIELD_VALUE) {
      ESP_LOGW(TAG, "Invalid numeric field in state response");
      return;
    }
    values[value_count++] = static_cast<int>(value);
    if (*end == '\0') {
      break;
    }
    if (*end != ',') {
      ESP_LOGW(TAG, "Invalid separator in state response");
      return;
    }
    cursor = end + 1;
  }

  if (value_count != SINGLE_ZONE_STATE_FIELD_COUNT && value_count != DUAL_ZONE_STATE_FIELD_COUNT) {
    ESP_LOGW(TAG, "Unexpected state field count: %u", static_cast<unsigned>(value_count));
    return;
  }

  this->schedule_state_timeout_();

  if (this->protocol_state_sensor_ != nullptr) {
    this->protocol_state_sensor_->publish_state(frame.c_str(), frame.size());
  }
  if (this->power_sensor_ != nullptr &&
      (values[STATE_FIELD_POWER] == 0 || values[STATE_FIELD_POWER] == 1)) {
    this->power_state_ = values[STATE_FIELD_POWER];
    this->power_sensor_->publish_state(values[STATE_FIELD_POWER] == 1);
  } else if (values[STATE_FIELD_POWER] == 0 || values[STATE_FIELD_POWER] == 1) {
    this->power_state_ = values[STATE_FIELD_POWER];
  }
  if (this->left_temperature_sensor_ != nullptr) {
    float temperature_c = values[STATE_FIELD_LEFT_TEMPERATURE];
    if (values[STATE_FIELD_TEMPERATURE_UNIT] == 2) {
      temperature_c = (temperature_c - FAHRENHEIT_OFFSET) * CELSIUS_CONVERSION_NUMERATOR /
              CELSIUS_CONVERSION_DENOMINATOR;
    }
    this->left_temperature_sensor_->publish_state(temperature_c);
  }
  if (this->climate_ != nullptr) {
    float temperature_c = values[STATE_FIELD_LEFT_TEMPERATURE];
    if (values[STATE_FIELD_TEMPERATURE_UNIT] == 2) {
      temperature_c = (temperature_c - FAHRENHEIT_OFFSET) * CELSIUS_CONVERSION_NUMERATOR /
              CELSIUS_CONVERSION_DENOMINATOR;
    }
    this->climate_->current_temperature = temperature_c;
    this->climate_->target_temperature = values[STATE_FIELD_POWER] == 1
                                             ? values[STATE_FIELD_LEFT_TARGET_TEMPERATURE]
                                             : std::numeric_limits<float>::quiet_NaN();
    this->climate_->mode = values[STATE_FIELD_POWER] == 1 ? climate::CLIMATE_MODE_COOL : climate::CLIMATE_MODE_OFF;
    this->climate_->publish_state();
  }
  if (values[STATE_FIELD_MODEL] == 2 && this->right_temperature_sensor_ != nullptr) {
    float temperature_c = values[STATE_FIELD_RIGHT_TEMPERATURE];
    if (values[STATE_FIELD_TEMPERATURE_UNIT] == 2) {
      temperature_c = (temperature_c - FAHRENHEIT_OFFSET) * CELSIUS_CONVERSION_NUMERATOR /
              CELSIUS_CONVERSION_DENOMINATOR;
    }
    this->right_temperature_sensor_->publish_state(temperature_c);
  }
  if (this->left_target_temperature_sensor_ != nullptr) {
    this->left_target_temperature_sensor_->publish_state(values[STATE_FIELD_LEFT_TARGET_TEMPERATURE]);
  }
  if (this->temperature_unit_control_ != nullptr) {
    if (values[STATE_FIELD_TEMPERATURE_UNIT] == 1) {
      this->temperature_unit_control_->publish_state("celsius");
    } else if (values[STATE_FIELD_TEMPERATURE_UNIT] == 2) {
      this->temperature_unit_control_->publish_state("fahrenheit");
    }
  }
  if (values[STATE_FIELD_MODEL] == 2 && this->right_target_temperature_sensor_ != nullptr) {
    this->right_target_temperature_sensor_->publish_state(values[STATE_FIELD_RIGHT_TARGET_TEMPERATURE]);
  }
  if (this->voltage_sensor_ != nullptr) {
    this->voltage_sensor_->publish_state(values[STATE_FIELD_RAW_VOLTAGE] / RAW_VOLTAGE_DIVISOR);
  }
  if (this->battery_percentage_sensor_ != nullptr) {
    const int voltage = values[STATE_FIELD_RAW_VOLTAGE];
        const int percentage = voltage <= BATTERY_0_PERCENT_MAX_RAW_VOLTAGE
               ? 0
               : voltage <= BATTERY_20_PERCENT_MAX_RAW_VOLTAGE
                 ? BATTERY_PERCENTAGE_STEP
                 : voltage <= BATTERY_40_PERCENT_MAX_RAW_VOLTAGE
                   ? BATTERY_PERCENTAGE_STEP * 2
                   : voltage <= BATTERY_60_PERCENT_MAX_RAW_VOLTAGE
                     ? BATTERY_PERCENTAGE_STEP * 3
                     : voltage <= BATTERY_80_PERCENT_MAX_RAW_VOLTAGE
                       ? BATTERY_PERCENTAGE_STEP * 4
                       : 100;
    this->battery_percentage_sensor_->publish_state(percentage);
  }
  if (this->battery_protection_sensor_ != nullptr) {
    switch (values[STATE_FIELD_BATTERY_PROTECTION]) {
      case 1:
        this->battery_protection_sensor_->publish_state(BATTERY_PROTECTION_LOW);
        if (this->battery_protection_control_ != nullptr) {
          this->battery_protection_control_->publish_state(BATTERY_PROTECTION_LOW);
        }
        break;
      case 2:
        this->battery_protection_sensor_->publish_state(BATTERY_PROTECTION_MIDDLE);
        if (this->battery_protection_control_ != nullptr) {
          this->battery_protection_control_->publish_state(BATTERY_PROTECTION_MIDDLE);
        }
        break;
      case 3:
        this->battery_protection_sensor_->publish_state(BATTERY_PROTECTION_HIGH);
        if (this->battery_protection_control_ != nullptr) {
          this->battery_protection_control_->publish_state(BATTERY_PROTECTION_HIGH);
        }
        break;
      default:
        break;
    }
  } else if (this->battery_protection_control_ != nullptr) {
    if (values[STATE_FIELD_BATTERY_PROTECTION] == 1) {
      this->battery_protection_control_->publish_state(BATTERY_PROTECTION_LOW);
    } else if (values[STATE_FIELD_BATTERY_PROTECTION] == 2) {
      this->battery_protection_control_->publish_state(BATTERY_PROTECTION_MIDDLE);
    } else if (values[STATE_FIELD_BATTERY_PROTECTION] == 3) {
      this->battery_protection_control_->publish_state(BATTERY_PROTECTION_HIGH);
    }
  }
  if (this->compressor_mode_sensor_ != nullptr) {
    if (values[STATE_FIELD_COMPRESSOR_MODE] == 1) {
      this->compressor_mode_sensor_->publish_state(COMPRESSOR_MODE_ECO);
      if (this->compressor_mode_control_ != nullptr) {
        this->compressor_mode_control_->publish_state(COMPRESSOR_MODE_ECO);
      }
    } else if (values[STATE_FIELD_COMPRESSOR_MODE] == 2) {
      this->compressor_mode_sensor_->publish_state(COMPRESSOR_MODE_MAX);
      if (this->compressor_mode_control_ != nullptr) {
        this->compressor_mode_control_->publish_state(COMPRESSOR_MODE_MAX);
      }
    }
  } else if (this->compressor_mode_control_ != nullptr) {
    if (values[STATE_FIELD_COMPRESSOR_MODE] == 1) {
      this->compressor_mode_control_->publish_state(COMPRESSOR_MODE_ECO);
    } else if (values[STATE_FIELD_COMPRESSOR_MODE] == 2) {
      this->compressor_mode_control_->publish_state(COMPRESSOR_MODE_MAX);
    }
  }

  if (values[STATE_FIELD_POWER] == 1 && std::isfinite(this->pending_target_temperature_)) {
    const float temperature = this->pending_target_temperature_;
    this->pending_target_temperature_ = std::numeric_limits<float>::quiet_NaN();
    this->set_timeout("climate_target_after_power_on", CONTROL_REFRESH_DELAY_MS,
                      [this, temperature]() { this->control_target_temperature(temperature); });
  }

  ESP_LOGD(TAG, "Parsed fridge state: model=%d power=%d temp=%d voltage=%.1fV", values[STATE_FIELD_MODEL],
           values[STATE_FIELD_POWER], values[STATE_FIELD_LEFT_TEMPERATURE],
           values[STATE_FIELD_RAW_VOLTAGE] / RAW_VOLTAGE_DIVISOR);
}

}  // namespace tiefu_fridge
}  // namespace esphome