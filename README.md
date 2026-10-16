# TieFu Fridge ESPHome Component

The component connects to a compatible fridge through ESPHome's `ble_client`. Network transport is configured separately by the ESPHome node, so the same component can be used over WiFi or a supported cellular connection.

## Compatibility

The minimum supported ESPHome version is **2026.2.4**. Earlier versions have not been verified. Entity icons and diagnostic metadata are configured through Python schemas, which have been validated with ESPHome 2026.2.4 and 2026.11.0-dev; this does not mean that every later version has been tested.

This component implements the BLE protocol used by the Android **Smart Car Fridge** app (`com.scinan.TieFuZX.Refrigerator`), whose APK is available [here](http://fir.gdtiefu.com/2knc) or [here](https://fir.xcxwo.com/2knc): service `FFF0`, write characteristic `FFF1`, and notification characteristic `FFF4`, including its `/S00` state frames and control commands.

The following fridge has been tested with this component and the app:

| Brand | Model | Status | Notes |
| --- | --- | --- | --- |
| Joytutus | 20 L CR02001 | Tested | Single compartment (`left` in the BLE protocol). The climate entity reports its temperature and controls its setpoint and fridge power. |

Compatibility is based on the BLE protocol, not the badge on the fridge. Other brands or rebadged models may work if they are controlled by this app and use the same protocol, but they have not been verified. The app identifies the TieFu product family. TieFu's manufacturer describes itself as an automotive/outdoor-products producer and advertises OEM/ODM services ([company profile](https://www.gdtiefu.com/en/about_7/)), but it does not publish which third-party fridge models use this app or BLE protocol.

Have a fridge that works with this component, or want to help test and improve it? You're very welcome to open an [issue](https://github.com/jaaviercr/esphome-tiefu-fridge/issues) with your fridge's brand and model and what you tested, or submit a pull request. I'm happy to add tested models to this list.

## Installation

Add this external component to your ESPHome node:

```yaml
external_components:
  - source: github://jaaviercr/esphome-tiefu-fridge
    components: [tiefu_fridge]
```

Configure the BLE client and the entities you want under `tiefu_fridge`:

```yaml
ble_client:
  - mac_address: !secret fridge_mac_address
    id: fridge_ble
    auto_connect: true

tiefu_fridge:
  id: fridge
  ble_client_id: fridge_ble
  update_interval: 30s
  voltage:
    name: "Fridge Voltage"
  battery_percentage:
    name: "Fridge Battery Percentage"
  fault_event:
    name: "Fridge Fault Event"
  climate:
    name: "Fridge Climate"
  compressor_mode_control:
    name: "Fridge Compressor Mode"
  battery_protection_control:
    name: "Fridge Battery Protection"
  temperature_unit_control:
    name: "Fridge Temperature Unit"
```

For a complete ESP32 node configuration, see [`examples/example.yaml`](examples/example.yaml). It uses the generic `esp32dev` board with ESP-IDF as a starting point; change the board and, when needed, the variant to match your hardware. Create a local `examples/secrets.yaml` from [`examples/secrets.yaml.example`](examples/secrets.yaml.example), then set your WiFi credentials and fridge MAC. The `secrets.yaml` file is ignored by Git.

For the verified Windows/ESP32-C6 test setup and repeatable validation commands, see [`DEVELOPMENT.md`](DEVELOPMENT.md).

`ble_client_id` is required. `update_interval` is optional and defaults to `30s`; the component also accepts ESPHome's generic `setup_priority` option.

The following read-only entities are optional. They expose state values already represented by controls, plus the latest decoded fault. Add any desired entries to the existing `tiefu_fridge:` block above; do not create a second block with the same YAML key:

```yaml
tiefu_fridge:
  # Read-only state duplicates of controls from the main example.
  power:
    name: "Fridge Power State"
  compressor_mode:
    name: "Fridge Compressor Mode State"
  battery_protection:
    name: "Fridge Battery Protection State"

  # Raw protocol frame and most recent decoded fault.
  protocol_state:
    name: "Fridge Protocol State"
  last_fault:
    name: "Fridge Last Fault"
```

When configured, `protocol_state` appears in Home Assistant under **Diagnostics** and is disabled by default; enable it from the entity settings if needed. The other optional read-only entities above are not automatically categorized as Diagnostics.

The component supplies contextual Material Design Icons by default (for example, `mdi:fridge` for the climate and `mdi:code-tags` for protocol state). Set `icon:` inside an entity's YAML block to override its default. Battery percentage uses Home Assistant's battery device-class icon.

The climate entity supports `off` and `cool`, reports current temperature, and controls a target in the fridge's Celsius range from -22 to 10 regardless of the selected display unit. Home Assistant hides the target temperature while the climate is off; the fridge's stored setpoint is reported again when it is on. Compressor mode options are `eco` and `max`; battery protection options are `low`, `middle`, and `high`; temperature unit options are `celsius` and `fahrenheit`.

`battery_percentage` reproduces the original app's voltage-based estimate: raw voltage `<= 96` maps to 0%, `<= 103` to 20%, `<= 111` to 40%, `<= 117` to 60%, `<= 124` to 80%, and higher values to 100%. This is an estimate from voltage, not a direct battery state-of-charge reading.

`fault_event` emits the event type `e1` through `e6` for Home Assistant automations when a live `/SEE` fault arrives. The optional `last_fault` text sensor retains the most recent decoded description for diagnostics. It only records faults received while this ESPHome node is connected; historical notices stored locally by the Android app cannot be retrieved from the fridge. No fridge faults are simulated by the component.

**Caution:** The original app warns that `low` battery protection can drain the vehicle's starter battery. Choose this option only when the power source is suitable.

The component updates reported values from complete status responses. Target-only climate changes are ignored while the fridge is off or its power state is unknown. Selecting `cool` while the fridge is off or its power state is unknown requests power-on; if the request also includes a target, the target is applied after the fridge reports that it is on. The battery protection, compressor mode, and temperature unit selectors ignore changes while the fridge is off or its power state is unknown. Selecting climate mode `off` powers it down and clears any pending target.

When the BLE link closes, or three `update_interval`s pass without a valid state response after BLE connects, the component clears cached sensor and select states rather than leaving stale readings. Home Assistant reports those entities as `unavailable` over the native API. Over MQTT, the component clears state topics with `None` or empty payloads; these do not mark MQTT entities unavailable, so numeric entities appear as `unknown` (other entity types may show an empty or unknown state). The climate is reset to `off` with unknown temperatures.

## Author

Developed and maintained by [JaavierCR](https://github.com/jaaviercr).

## License

Licensed under the [Apache License 2.0](LICENSE).

## Trademarks and disclaimer

This is an independent community project and is not affiliated with, endorsed by, sponsored by, or supported by TieFu, Guangdong Minghua Auto Equipment Technology Co., Joytutus, Scinan, or the Smart Car Fridge app developer. Product, company, and app names are used only to identify the devices and protocol with which this component is intended to interoperate. The software is provided "as is", without warranty.
