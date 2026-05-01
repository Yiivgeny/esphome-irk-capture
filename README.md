# ESPHome IRK capture component

External ESPHome component for capturing a peer BLE Identity Resolving Key (IRK)
through the ESPHome BLE stack.

The component code is inspired by the implementation in
[ESPresense](https://github.com/ESPresense/ESPresense) and provides similar
BLE Enroll flow for capturing a peer IRK.

As of 2026-05-01, the latest stable ESPHome release is `2026.4.3`:
- GitHub releases: <https://github.com/esphome/esphome/releases>
- Developer docs: <https://developers.esphome.io/contributing/development-environment/>

## Repository contents

- `uv`-managed Python environment pinned to Python `3.11.13`
- ESPHome pinned to `2026.4.3`
- `irk_capture` external component in `components/irk_capture/`
- example config in `examples/irk_capture.yaml`

## Quick start

```bash
uv sync
uv run esphome compile examples/irk_capture.yaml
```

## IRK capture

The component adds an ESPHome switch that temporarily exposes a BLE GATT server,
accepts an incoming BLE pairing, and emits an automation event with the discovered IRK.

Minimal YAML shape:

```yaml
esp32_ble:
  id: ble_core
  io_capability: none
  auth_req_mode: bond
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

Behavior:

- turning the `enroll_switch` on starts advertising a connectable BLE service
- turning the optional `visible_switch` on exposes the BLE service without enabling IRK enrollment
- on incoming connection, the component requests BLE encryption and bonding
- when the peer shares its Identity Resolving Key, `on_irk` fires with:
  - `irk`: 32-char lowercase hex IRK
  - `address`: identity/static BLE address when available
- after success, enrollment mode turns itself off and the client is disconnected

## Project layout

```text
components/
  irk_capture/
    __init__.py
    irk_capture.h
    irk_capture.cpp
examples/
  irk_capture.yaml
```

Reference local components in YAML with:

```yaml
external_components:
  - source:
      type: local
      path: ../components
```
