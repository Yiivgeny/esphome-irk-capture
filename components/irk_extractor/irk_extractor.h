#pragma once

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_descriptor.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/components/esp32_ble_server/ble_service.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef USE_ESP32

#include <esp_gap_ble_api.h>

namespace esphome::irk_extractor {

class IrkExtractor;

class IrkFoundTrigger : public Trigger<std::string, std::string> {};

class IrkExtractorSwitch : public switch_::Switch {
 public:
  explicit IrkExtractorSwitch(IrkExtractor *parent) : parent_(parent) {}

 protected:
  void write_state(bool state) override;
  IrkExtractor *parent_;
};

class IrkExtractor : public Component, public Parented<esp32_ble_server::BLEServer> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

  void set_auto_disconnect(bool auto_disconnect) { this->auto_disconnect_ = auto_disconnect; }
  void set_enroll_switch(IrkExtractorSwitch *enroll_switch) { this->enroll_switch_ = enroll_switch; }
  void add_on_irk_trigger(IrkFoundTrigger *trigger) { this->irk_triggers_.push_back(trigger); }

  void set_enabled(bool enabled);
  bool is_enabled() const { return this->enabled_; }

 protected:
  void ensure_service_();
  void sync_server_state_();
  void sync_advertising_mode_();
  void disconnect_all_clients_();
  void handle_gatts_event_(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
  void handle_gap_event_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

  void handle_pid_key_(const esp_ble_key_t &ble_key);
  void handle_auth_complete_(const esp_ble_auth_cmpl_t &auth_cmpl);

  static std::string format_address_(const esp_bd_addr_t address);
  static std::string format_irk_(const uint8_t *irk);
  bool lookup_irk_from_bond_db_(const esp_bd_addr_t address, std::string *irk, std::string *identity_address);
  void emit_irk_(const std::string &irk, const std::string &address, const std::string &peer_address);

  struct PendingIrk {
    std::string irk;
    std::string identity_address;
  };

  bool enabled_{false};
  bool auto_disconnect_{true};
  bool service_started_{false};

  esp32_ble_server::BLEService *heart_rate_service_{nullptr};
  esp32_ble_server::BLECharacteristic *heart_rate_measurement_{nullptr};
  esp32_ble_server::BLEDescriptor *cccd_{nullptr};
  IrkExtractorSwitch *enroll_switch_{nullptr};
  std::vector<IrkFoundTrigger *> irk_triggers_;

  std::unordered_map<std::string, uint16_t> conn_ids_by_peer_;
  std::unordered_map<std::string, PendingIrk> pending_irks_by_peer_;
  std::set<std::string> emitted_irks_;
};

}  // namespace esphome::irk_extractor

#endif
