#include "irk_extractor.h"

#include "esphome/core/log.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstring>
#include <vector>

#include <esp_bt_defs.h>
#include <esp_gatt_defs.h>
#include <esp_gatts_api.h>

namespace esphome::irk_extractor {

static const char *const TAG = "irk_extractor";

void IrkExtractorSwitch::write_state(bool state) { this->parent_->set_enabled(state); }

void IrkExtractor::setup() {
  this->ensure_service_();

  if (this->enroll_switch_ != nullptr) {
    this->enroll_switch_->publish_state(this->enabled_);
  }
}

void IrkExtractor::loop() { this->sync_server_state_(); }

void IrkExtractor::dump_config() {
  ESP_LOGCONFIG(TAG, "IRK Extractor:");
  ESP_LOGCONFIG(TAG, "  Enabled: %s", YESNO(this->enabled_));
  ESP_LOGCONFIG(TAG, "  Auto disconnect: %s", YESNO(this->auto_disconnect_));
  LOG_SWITCH("  ", "Enroll Switch", this->enroll_switch_);
}

float IrkExtractor::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH + 20.0f; }

void IrkExtractor::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  this->handle_gap_event_(event, param);
}

void IrkExtractor::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t *param) {
  this->handle_gatts_event_(event, gatts_if, param);
}

void IrkExtractor::set_enabled(bool enabled) {
  if (this->enabled_ == enabled) {
    if (this->enroll_switch_ != nullptr) {
      this->enroll_switch_->publish_state(enabled);
    }
    return;
  }

  this->enabled_ = enabled;
  ESP_LOGI(TAG, "%s IRK enrollment mode", enabled ? "Enabling" : "Disabling");

  if (!enabled) {
    this->pending_irks_by_peer_.clear();
    if (this->auto_disconnect_) {
      this->disconnect_all_clients_();
    }
  }

  this->sync_server_state_();

  if (this->enroll_switch_ != nullptr) {
    this->enroll_switch_->publish_state(enabled);
  }
}

void IrkExtractor::ensure_service_() {
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
  this->heart_rate_measurement_->set_value({0x06, 0x48});

  this->cccd_ = new esp32_ble_server::BLEDescriptor(  // NOLINT(cppcoreguidelines-owning-memory)
      esp32_ble::ESPBTUUID::from_uint16(ESP_GATT_UUID_CHAR_CLIENT_CONFIG), 2, true, true);
  this->cccd_->set_value({0x00, 0x00});
  this->heart_rate_measurement_->add_descriptor(this->cccd_);
}

void IrkExtractor::sync_server_state_() {
  if (this->heart_rate_service_ == nullptr || !this->parent_->is_running()) {
    return;
  }

  if (this->enabled_) {
    if (!this->service_started_ && !this->heart_rate_service_->is_running() && !this->heart_rate_service_->is_starting()) {
      this->heart_rate_service_->start();
      this->service_started_ = true;
    }
  } else if (this->service_started_) {
    this->heart_rate_service_->stop();
    this->service_started_ = false;
  }
}

void IrkExtractor::disconnect_all_clients_() {
  auto entries = this->conn_ids_by_peer_;
  for (const auto &[peer_address, conn_id] : entries) {
    esp_err_t err = esp_ble_gatts_close(this->parent_->get_gatts_if(), conn_id);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to disconnect %s: %s", peer_address.c_str(), esp_err_to_name(err));
    }
  }
}

void IrkExtractor::handle_gatts_event_(esp_gatts_cb_event_t event, esp_gatt_if_t, esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_CONNECT_EVT: {
      if (!this->enabled_) {
        break;
      }
      const std::string peer_address = format_address_(param->connect.remote_bda);
      this->conn_ids_by_peer_[peer_address] = param->connect.conn_id;
      ESP_LOGI(TAG, "BLE client connected: %s (conn_id=%u)", peer_address.c_str(), param->connect.conn_id);
      esp_err_t err = esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
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

void IrkExtractor::handle_gap_event_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
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

void IrkExtractor::handle_pid_key_(const esp_ble_key_t &ble_key) {
  const std::string peer_address = format_address_(ble_key.bd_addr);
  const std::string irk = format_irk_(ble_key.p_key_value.pid_key.irk);
  const std::string identity_address = format_address_(ble_key.p_key_value.pid_key.static_addr);

  this->pending_irks_by_peer_[peer_address] = PendingIrk{
      irk,
      identity_address,
  };

  ESP_LOGI(TAG, "Received peer IRK for %s", peer_address.c_str());
}

void IrkExtractor::handle_auth_complete_(const esp_ble_auth_cmpl_t &auth_cmpl) {
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

std::string IrkExtractor::format_address_(const esp_bd_addr_t address) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2], address[3],
           address[4], address[5]);
  return buffer;
}

std::string IrkExtractor::format_irk_(const uint8_t *irk) {
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

bool IrkExtractor::lookup_irk_from_bond_db_(const esp_bd_addr_t address, std::string *irk, std::string *identity_address) {
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

void IrkExtractor::emit_irk_(const std::string &irk, const std::string &address, const std::string &peer_address) {
  if (!this->emitted_irks_.insert(irk).second) {
    ESP_LOGD(TAG, "IRK already emitted for %s", peer_address.c_str());
    return;
  }

  ESP_LOGI(TAG, "Discovered IRK %s for %s", irk.c_str(), address.c_str());
  for (auto *trigger : this->irk_triggers_) {
    trigger->trigger(irk, address);
  }

  if (this->auto_disconnect_) {
    auto it = this->conn_ids_by_peer_.find(peer_address);
    if (it != this->conn_ids_by_peer_.end()) {
      esp_err_t err = esp_ble_gatts_close(this->parent_->get_gatts_if(), it->second);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disconnect %s after IRK extraction: %s", peer_address.c_str(), esp_err_to_name(err));
      }
    }
  }

  this->enabled_ = false;
  this->sync_server_state_();
  if (this->enroll_switch_ != nullptr) {
    this->enroll_switch_->publish_state(false);
  }
}

}  // namespace esphome::irk_extractor

#endif
