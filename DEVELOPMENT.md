# Development and Testing

This guide applies to any node configuration that uses this component. The target board, variant, and framework come from the ESPHome YAML file; this guide does not prescribe a particular board.

## Prerequisites

- Python **3.13** and ESPHome **2026.2.4** (the version used to validate this component).
- A complete Git installation available on `PATH`; PlatformIO uses Git for ESP32 packages, and configurations using `github://` external components also need Git's HTTPS transport helper (`git-remote-https`).
- Network access for the first build and whenever ESPHome must fetch an external component or toolchain.

The component uses ESPHome's `ble_client`, `esp32_ble_tracker`, and ESP-IDF GATT client support. Arduino framework builds are not the tested configuration. ESPHome's `api`, OTA, WiFi, BLE, and board-specific settings belong in the node YAML.

## Python Environment

Create a project-local virtual environment and install the tested ESPHome version. On Windows, use a standalone CPython installation rather than the Microsoft Store alias; PlatformIO must create an ESP-IDF Python environment from it.

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install esphome==2026.2.4
```

On macOS or Linux:

```sh
python3.13 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install esphome==2026.2.4
```

On Windows networks that inspect TLS using a certificate trusted by Windows, install `pip-system-certs` in the virtual environment so Python uses the Windows certificate store. This preserves TLS certificate verification; do not disable SSL verification.

Create a local secrets file from the example and put WiFi credentials, fallback AP password, and any device identifiers there. Never commit real secrets.

## Validate and Build

Run commands from the project root and replace `<node.yaml>` with the configuration to build. On Windows, use the virtual environment's Python executable:

```powershell
\.venv\Scripts\python.exe -m esphome config <node.yaml>
\.venv\Scripts\python.exe -m esphome compile <node.yaml>
```

On macOS or Linux, use `.venv/bin/python` instead. PlatformIO needs Git on `PATH`. If ESP-IDF extraction fails because of Windows path length, set a short per-user PlatformIO core directory in the current PowerShell window:

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path $env:USERPROFILE 'pio-core'
```

The variable applies only to that PowerShell window. ESPHome writes generated files under `.esphome/` beside the node YAML; keep generated builds and secrets out of version control.

If CMake reports `Multiple ways to build the same target` with duplicate ESP-IDF source paths, clean only that node's generated build and compile again:

```powershell
\.venv\Scripts\python.exe -m esphome clean <node.yaml>
\.venv\Scripts\python.exe -m esphome compile <node.yaml>
```

`clean` removes generated files and the PlatformIO download cache for the build, but leaves source files and secrets untouched.

## Upload

For a first USB flash, pass the serial port detected by the operating system. The `run` command compiles, uploads, then keeps a log monitor open; press `Ctrl+C` to exit the monitor after checking startup.

```powershell
\.venv\Scripts\python.exe -m esphome run <node.yaml> --device COMx
```

After WiFi and ESPHome OTA are configured and working, use the node's reachable hostname instead:

```powershell
\.venv\Scripts\python.exe -m esphome run <node.yaml> --device <node-name>.local
```

To upload an already-built firmware without starting the monitor, use `esphome upload <node.yaml> --device <device>`. Use `esphome logs <node.yaml> --device <device>` to open logs separately. Replace `.venv\Scripts\python.exe` with `.venv/bin/python` on macOS or Linux.

If the uploader reports that the detected chip differs from the selected target, check the YAML's board and variant against the actual hardware before retrying. If a serial port is busy, close other serial monitors or terminal sessions using it.

## Post-upload Checks

1. Confirm the uploader reports a successful write and the device reboots without a boot loop.
2. Inspect ESPHome logs and confirm WiFi connects and the API becomes available.
3. In Home Assistant, confirm the node connects and the entities configured in the YAML appear.
4. Confirm the fridge connects over BLE and reported values update. This component expects service `FFF0`, writes to characteristic `FFF1`, and receives notifications on `FFF4`.
5. Compare temperature, power, and target with the companion app. The climate target is hidden while the fridge is off. A `cool` request can power it on; avoid undocumented control commands.

`Fridge Protocol State` is a diagnostic entity disabled by default; enable it in Home Assistant when raw `/S00/H` frames are needed. There is no automated unit-test suite, so `esphome config` and `esphome compile` are the repeatable software validation gates. Hardware and Home Assistant checks must be performed on the actual node.
