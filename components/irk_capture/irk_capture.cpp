#include "irk_capture.h"

#include "esphome/core/log.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>
#include <vector>

#include <esp_bt_defs.h>
#include <esp_gatt_defs.h>
#include <esp_gatts_api.h>

namespace esphome::irk_capture {

static const char *const TAG = "irk_capture";

class BLECharacteristicAccess : public esp32_ble_server::BLECharacteristic {
 public:
  using esp32_ble_server::BLECharacteristic::permissions_;

  static void set_permissions(esp32_ble_server::BLECharacteristic *characteristic, esp_gatt_perm_t permissions) {
    reinterpret_cast<BLECharacteristicAccess *>(characteristic)->permissions_ = permissions;
  }
};

void IrkCaptureEnrollSwitch::write_state(bool state) { this->parent_->set_enabled(state); }

void IrkCaptureVisibleSwitch::write_state(bool state) { this->parent_->set_visible(state); }

void IrkCapture::setup() {
  this->ensure_service_();
  this->configure_security_profile_();
  this->sync_advertising_mode_();

  if (this->visible_switch_ != nullptr) {
    this->visible_switch_->publish_state(this->visible_);
  }
  if (this->enroll_switch_ != nullptr) {
    this->enroll_switch_->publish_state(this->enabled_);
  }
}

void IrkCapture::loop() {
  this->configure_security_profile_();
  this->sync_server_state_();
  this->maybe_notify_heart_rate_();
}

void IrkCapture::dump_config() {
  ESP_LOGCONFIG(TAG, "IRK Capture:");
  ESP_LOGCONFIG(TAG, "  Visible: %s", YESNO(this->visible_));
  ESP_LOGCONFIG(TAG, "  Enabled: %s", YESNO(this->enabled_));
  ESP_LOGCONFIG(TAG, "  Auto disconnect: %s", YESNO(this->auto_disconnect_));
  LOG_SWITCH("  ", "Visible Switch", this->visible_switch_);
  LOG_SWITCH("  ", "Enroll Switch", this->enroll_switch_);
}

float IrkCapture::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH + 20.0f; }

void IrkCapture::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  this->handle_gap_event_(event, param);
}

void IrkCapture::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t *param) {
  this->handle_gatts_event_(event, gatts_if, param);
}

void IrkCapture::set_enabled(bool enabled) { this->apply_state_(this->visible_ || enabled, enabled); }

void IrkCapture::set_visible(bool visible) { this->apply_state_(visible, visible ? this->enabled_ : false); }

void IrkCapture::apply_state_(bool visible, bool enabled) {
  if (enabled && !visible) {
    visible = true;
  }

  if (this->visible_ == visible && this->enabled_ == enabled) {
    if (this->visible_switch_ != nullptr) {
      this->visible_switch_->publish_state(visible);
    }
    if (this->enroll_switch_ != nullptr) {
      this->enroll_switch_->publish_state(enabled);
    }
    return;
  }

  const bool visible_changed = this->visible_ != visible;
  const bool enabled_changed = this->enabled_ != enabled;
  const bool should_disconnect = (!visible && this->visible_) || (!enabled && this->enabled_);

  if (visible_changed) {
    ESP_LOGI(TAG, "%s BLE visible mode", visible ? "Enabling" : "Disabling");
  }
  if (enabled_changed) {
    ESP_LOGI(TAG, "%s IRK enrollment mode", enabled ? "Enabling" : "Disabling");
  }

  if (enabled && !this->enabled_) {
    this->emitted_irks_.clear();
  }
  if (!enabled) {
    this->pending_irks_by_peer_.clear();
  }

  this->visible_ = visible;
  this->enabled_ = enabled;

  if (should_disconnect && this->auto_disconnect_) {
    this->disconnect_all_clients_();
  }

  this->sync_advertising_mode_();
  this->sync_server_state_();

  if (this->visible_switch_ != nullptr) {
    this->visible_switch_->publish_state(visible);
  }
  if (this->enroll_switch_ != nullptr) {
    this->enroll_switch_->publish_state(enabled);
  }
}

void IrkCapture::ensure_service_() {
  if (this->heart_rate_service_ != nullptr) {
    return;
  }

  this->heart_rate_service_ = this->parent_->create_service(
      esp32_ble::ESPBTUUID::from_uint16(0x180D), true, 4);
  if (this->heart_rate_service_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create Heart Rate service");
    this->mark_failed();
    return;
  }

  this->heart_rate_measurement_ = this->heart_rate_service_->create_characteristic(
      esp32_ble::ESPBTUUID::from_uint16(0x2A37),
      esp32_ble_server::BLECharacteristic::PROPERTY_READ | esp32_ble_server::BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristicAccess::set_permissions(this->heart_rate_measurement_, ESP_GATT_PERM_READ_ENCRYPTED);
  this->heart_rate_measurement_->set_value({0x06, 0x48});

  this->cccd_ = new esp32_ble_server::BLEDescriptor(  // NOLINT(cppcoreguidelines-owning-memory)
      esp32_ble::ESPBTUUID::from_uint16(ESP_GATT_UUID_CHAR_CLIENT_CONFIG), 2, true, true);
  this->cccd_->set_value({0x00, 0x00});
  this->heart_rate_measurement_->add_descriptor(this->cccd_);
}

void IrkCapture::configure_security_profile_() {
  if (this->security_profile_configured_ || esp32_ble::global_ble == nullptr || !esp32_ble::global_ble->is_active()) {
    return;
  }

  uint8_t key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_err_t err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_mask, sizeof(key_mask));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set BLE initiator key distribution: %s", esp_err_to_name(err));
    return;
  }

  err = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_mask, sizeof(key_mask));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set BLE responder key distribution: %s", esp_err_to_name(err));
    return;
  }

  this->security_profile_configured_ = true;
  ESP_LOGI(TAG, "Configured BLE security profile for IRK enrollment");
}

void IrkCapture::sync_server_state_() {
  if (this->heart_rate_service_ == nullptr || !this->parent_->is_running()) {
    return;
  }

  if (this->visible_) {
    if (!this->service_started_ && !this->heart_rate_service_->is_running() && !this->heart_rate_service_->is_starting()) {
      this->heart_rate_service_->start();
      this->service_started_ = true;
    }
  } else if (this->service_started_) {
    this->heart_rate_service_->stop();
    this->service_started_ = false;
  }
}

void IrkCapture::sync_advertising_mode_() {
  auto *server = this->get_parent();
  if (server == nullptr) {
    return;
  }

  auto *ble = server->get_parent();
  if (ble == nullptr || !ble->is_active()) {
    return;
  }

  ble->advertising_set_service_data_and_name({}, this->visible_);
  ESP_LOGI(TAG, "BLE advertising name %s in visible mode", this->visible_ ? "enabled" : "disabled");
}

void IrkCapture::maybe_notify_heart_rate_() {
  if (!this->enabled_ || this->heart_rate_measurement_ == nullptr || this->parent_ == nullptr ||
      this->parent_->get_connected_client_count() == 0) {
    return;
  }

  const uint32_t now = millis();
  if (now - this->last_heart_rate_notify_ms_ < 250) {
    return;
  }

  this->last_heart_rate_notify_ms_ = now;
  uint8_t value = micros() & 0xFF;
  this->heart_rate_measurement_->set_value({0x06, value});
  this->heart_rate_measurement_->notify();
}

void IrkCapture::update_connection_params_(const esp_bd_addr_t address) {
  esp_ble_conn_update_params_t params{};
  memcpy(params.bda, address, sizeof(params.bda));
  params.min_int = 0x06;
  params.max_int = 0x12;
  params.latency = 0;
  params.timeout = 400;

  esp_err_t err = esp_ble_gap_update_conn_params(&params);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to request BLE connection params update: %s", esp_err_to_name(err));
  }
}

void IrkCapture::disconnect_all_clients_() {
  auto entries = this->conn_ids_by_peer_;
  for (const auto &[peer_address, conn_id] : entries) {
    esp_err_t err = esp_ble_gatts_close(this->parent_->get_gatts_if(), conn_id);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to disconnect %s: %s", peer_address.c_str(), esp_err_to_name(err));
    }
  }
}

void IrkCapture::handle_gatts_event_(esp_gatts_cb_event_t event, esp_gatt_if_t, esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_CONNECT_EVT: {
      if (!this->enabled_) {
        break;
      }
      const std::string peer_address = format_address_(param->connect.remote_bda);
      this->conn_ids_by_peer_[peer_address] = param->connect.conn_id;
      ESP_LOGI(TAG, "BLE client connected: %s (conn_id=%u)", peer_address.c_str(), param->connect.conn_id);
      this->update_connection_params_(param->connect.remote_bda);
      esp_err_t err = esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request encryption for %s: %s", peer_address.c_str(), esp_err_to_name(err));
      }
      break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
      const std::string peer_address = format_address_(param->disconnect.remote_bda);
      this->conn_ids_by_peer_.erase(peer_address);
      this->pending_irks_by_peer_.erase(peer_address);
      ESP_LOGI(TAG, "BLE client disconnected: %s", peer_address.c_str());
      break;
    }
    default:
      break;
  }
}

void IrkCapture::handle_gap_event_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (!this->enabled_) {
    return;
  }

  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_KEY_EVT:
      if (param->ble_security.ble_key.key_type == ESP_LE_KEY_PID) {
        this->handle_pid_key_(param->ble_security.ble_key);
      }
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      this->handle_auth_complete_(param->ble_security.auth_cmpl);
      break;
    default:
      break;
  }
}

void IrkCapture::handle_pid_key_(const esp_ble_key_t &ble_key) {
  const std::string peer_address = format_address_(ble_key.bd_addr);
  const std::string irk = format_irk_(ble_key.p_key_value.pid_key.irk);
  const std::string identity_address = format_address_(ble_key.p_key_value.pid_key.static_addr);

  this->pending_irks_by_peer_[peer_address] = PendingIrk{
      irk,
      identity_address,
  };

  ESP_LOGI(TAG, "Received peer IRK for %s", peer_address.c_str());
}

void IrkCapture::handle_auth_complete_(const esp_ble_auth_cmpl_t &auth_cmpl) {
  const std::string peer_address = format_address_(auth_cmpl.bd_addr);
  if (!auth_cmpl.success) {
    ESP_LOGW(TAG, "BLE pairing failed for %s, reason=0x%02x", peer_address.c_str(), auth_cmpl.fail_reason);
    this->pending_irks_by_peer_.erase(peer_address);
    return;
  }

  std::string irk;
  std::string identity_address = peer_address;

  auto pending = this->pending_irks_by_peer_.find(peer_address);
  if (pending != this->pending_irks_by_peer_.end()) {
    irk = pending->second.irk;
    if (!pending->second.identity_address.empty() && pending->second.identity_address != "00:00:00:00:00:00") {
      identity_address = pending->second.identity_address;
    }
    this->pending_irks_by_peer_.erase(pending);
  } else if (!this->lookup_irk_from_bond_db_(auth_cmpl.bd_addr, &irk, &identity_address)) {
    ESP_LOGW(TAG, "Pairing completed for %s but no IRK was found", peer_address.c_str());
    return;
  }

  this->emit_irk_(irk, identity_address, peer_address);
}

std::string IrkCapture::format_address_(const esp_bd_addr_t address) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2], address[3],
           address[4], address[5]);
  return buffer;
}

std::string IrkCapture::format_irk_(const uint8_t *irk) {
  static const char hex_digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(32);
  for (int i = 0; i < 16; i++) {
    const auto c = irk[i];
    output.push_back(hex_digits[c >> 4]);
    output.push_back(hex_digits[c & 0x0F]);
  }
  return output;
}

bool IrkCapture::lookup_irk_from_bond_db_(const esp_bd_addr_t address, std::string *irk, std::string *identity_address) {
  int device_count = esp_ble_get_bond_device_num();
  if (device_count <= 0) {
    return false;
  }

  std::vector<esp_ble_bond_dev_t> devices(device_count);
  if (esp_ble_get_bond_device_list(&device_count, devices.data()) != ESP_OK) {
    return false;
  }

  for (int i = 0; i < device_count; i++) {
    const auto &device = devices[i];
    if (memcmp(device.bd_addr, address, ESP_BD_ADDR_LEN) != 0) {
      continue;
    }
    if ((device.bond_key.key_mask & ESP_LE_KEY_PID) == 0) {
      return false;
    }
    *irk = format_irk_(device.bond_key.pid_key.irk);
    *identity_address = format_address_(device.bond_key.pid_key.static_addr);
    return true;
  }

  return false;
}

void IrkCapture::emit_irk_(const std::string &irk, const std::string &address, const std::string &peer_address) {
  if (!this->emitted_irks_.insert(irk).second) {
    ESP_LOGI(TAG, "IRK for %s already emitted in this enroll session", peer_address.c_str());
  } else {
    ESP_LOGI(TAG, "Discovered IRK %s for %s", irk.c_str(), address.c_str());
    for (auto *trigger : this->irk_triggers_) {
      trigger->trigger(irk, address);
    }
  }

  if (this->auto_disconnect_) {
    auto it = this->conn_ids_by_peer_.find(peer_address);
    if (it != this->conn_ids_by_peer_.end()) {
      esp_err_t err = esp_ble_gatts_close(this->parent_->get_gatts_if(), it->second);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disconnect %s after IRK capturing: %s", peer_address.c_str(), esp_err_to_name(err));
      }
    }
  }

  this->apply_state_(this->visible_, false);
}

}  // namespace esphome::irk_capture

#endif
