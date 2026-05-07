# ESPHome IRK Capture

External ESPHome component for capturing a peer BLE Identity Resolving Key
(IRK) during BLE pairing and bonding on ESP32.

The component exposes a temporary BLE enrollment mode through an ESPHome switch
and emits an `on_irk` automation when the peer IRK is discovered. The
implementation is inspired by the BLE enroll flow used in
[ESPresense](https://github.com/ESPresense/ESPresense).

The primary use case for this component is enrolling devices for Home
Assistant's
[Private BLE Device](https://www.home-assistant.io/integrations/private_ble_device/)
integration.

## Requirements

- ESP32 target
- `framework: esp-idf`
- ESPHome `2026.4.x` compatibility, tested with `2026.4.3`
- `esp32_ble` and `esp32_ble_server` enabled in the node configuration

## Tested With

The component has been tested with the following peer platforms:

- iOS 15
- iOS 26
- watchOS 26
- Android 10

It has also been tested together with Home Assistant's
[`bluetooth_proxy`](https://www.home-assistant.io/integrations/bluetooth_proxy/)
integration.

## Installation

### From GitHub

Add the repository as an external component source:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Yiivgeny/esphome-irk-capture
      ref: main
    components: [irk_capture]
```

ESPHome expects git-based external components to live under a `components/`
directory, which matches this repository layout.

### From a local checkout

For local development, point ESPHome at the checked out repository:

```yaml
external_components:
  - source:
      type: local
      path: ../components
    components: [irk_capture]
```

## Minimal Configuration

```yaml
esp32_ble:
  id: ble_core
  name: "IRK Capture"
  io_capability: none
  auth_req_mode: sc_bond
  max_connections: 1

esp32_ble_server:
  id: ble_server
  manufacturer: "ESPHome"
  model: "IRK Capture"
  max_clients: 1

irk_capture:
  id: irk_capture_component
  ble_id: ble_core
  ble_server_id: ble_server
  enroll_switch:
    name: "BLE Enroll"
  on_irk:
    then:
      - logger.log:
          format: "Discovered IRK %s for %s"
          args: [irk.c_str(), address.c_str()]
```

The BLE device name is taken from `esp32_ble.name`. If omitted, ESPHome falls
back to the node hostname. Keeping the BLE name at 20 characters or less is a
safe default.

## Configuration Reference

### `irk_capture`

- `id`: Component ID.
- `ble_id`: ID of the `esp32_ble` component. Required.
- `ble_server_id`: ID of the `esp32_ble_server` component. Required.
- `auto_disable`: Disable enrollment mode automatically after a successful IRK
  capture. Defaults to `true`.
- `auto_disconnect`: Disconnect the active BLE client automatically after IRK
  capture. Defaults to `true`.
- `enroll_switch`: Optional switch that enables and disables enrollment mode.
- `on_irk`: Optional automation triggered when an IRK is captured.

### `on_irk` automation arguments

- `irk`: 32-character lowercase hexadecimal IRK string with bytes reversed
  from the little-endian value reported by the BLE stack.
- `address`: Peer identity or static BLE address when available.

## How It Works

1. Turn on the enrollment switch.
2. The node starts advertising a connectable BLE service.
3. A peer connects and the component requests encryption and bonding.
4. When the peer provides an identity key, the component resolves the IRK and
   fires `on_irk`.
5. After a successful capture, the component can automatically disconnect the
   client and disable enrollment mode.

Within a single enrollment session, duplicate IRKs are emitted only once.

## Examples

- [`examples/irk_capture.yaml`](/Users/evgeny/Projects/esphome-irk-extractor/examples/irk_capture.yaml):
  minimal configuration
- [`examples/m5atom_lite.yaml`](/Users/evgeny/Projects/esphome-irk-extractor/examples/m5atom_lite.yaml):
  M5Atom Lite example with button control, status LED, and Home Assistant event

## Limitations and Notes

- ESP32 only.
- Designed for IRK enrollment flow, not as a general-purpose BLE service.
- A peer must support pairing and bonding in a way that exposes the identity
  resolving key.
- Some peers may complete pairing without yielding a usable IRK, in which case
  `on_irk` will not fire.
- The component relies on `esp32_ble` and `esp32_ble_server` behavior in
  ESPHome `2026.4.x`.

## Development

The repository uses an `uv`-managed Python environment. The exact tested
dependency set is recorded in `uv.lock`.

```bash
uv sync
uv run esphome compile examples/irk_capture.yaml
uv run esphome compile examples/m5atom_lite.yaml
```

## Repository Layout

```text
components/
  irk_capture/
    __init__.py
    irk_capture.h
    irk_capture.cpp
examples/
  irk_capture.yaml
  m5atom_lite.yaml
```
