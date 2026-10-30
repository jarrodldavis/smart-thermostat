# Smart Thermostat for ESPHome

ESPHome firmware for a touchscreen thermostat with local HVAC control, Home
Assistant integration, Device Builder adoption, and OTA updates. Supports
conventional heating/cooling and heat pumps with one or two compressor stages,
electric supplemental heat, or replacement furnace backup.

Targets the V0.7.3 ESP32-S3 prototype: 16 MB flash, no PSRAM, ILI9341 display,
XPT2046 touch, AHT20 temperature/humidity sensor, and LD2410 presence sensor.

## Build and flash

Run these commands from the repository root. Use Python 3.12–3.14 (CI uses 3.13),
Git, and a USB data cable. The first build downloads the ESP32 toolchain.

```sh
python3.13 -m venv /tmp/smart-thermostat-venv
. /tmp/smart-thermostat-venv/bin/activate
pip install esphome==2026.10.0
esphome config esphome/smart-thermostat.yaml
esphome run esphome/smart-thermostat.yaml --device /dev/ttyACM0
```

Replace `/dev/ttyACM0` with the board's serial port. `run` builds, uploads, and
opens logs. To open logs later:

```sh
esphome logs esphome/smart-thermostat.yaml --device /dev/ttyACM0
```

**Disconnect the board from HVAC wiring before flashing or testing.** Back up
existing firmware and record equipment settings before erasing flash. For an
initial installation, use USB and erase flash before uploading.

Alternatively, flash a published factory image with ESPHome Web or the project's
GitHub Pages installer. Factory firmware starts uncommissioned: the display and
sensors operate, but HVAC outputs remain inhibited until setup is confirmed.

## Connect Wi-Fi and Home Assistant

1. Provision Wi-Fi over USB with serial Improv, or open **Diagnostics → Wi-Fi
   setup** on the touchscreen. Scan the QR code to join the setup network.
   If needed, open `http://192.168.4.1/` to enter your home network credentials.
2. For manual entry, use the displayed SSID and password, joining both password
   lines without a space. The setup password changes on reboot; saved home Wi-Fi
   credentials remain. The fallback AP starts when Wi-Fi is unavailable; allow
   about 90 seconds when previously saved credentials cannot connect.
3. In Home Assistant, add the discovered ESPHome integration under **Settings →
   Devices & services**.
4. Separately adopt the device in **ESPHome Device Builder** to create an editable
   configuration. Configure your own Wi-Fi credentials and API encryption key,
   then install that configuration. See [`examples/adopted.yaml`](examples/adopted.yaml).

Adoption requires a published release tag. For development builds, use the local
entrypoint, `esphome/smart-thermostat.yaml`. Store Wi-Fi credentials, the API
encryption key, and an optional OTA password in your local `secrets.yaml`.

Local HVAC operation continues without Wi-Fi or Home Assistant. In safe mode,
use saved Wi-Fi credentials or USB recovery; the touchscreen is unavailable.

## Configure and commission

With equipment disconnected, use the touchscreen **Settings** menu or Home
Assistant configuration entities:

1. Choose Conventional or Heat pump and one or two compressor stages.
2. For a heat pump, choose None, Electric supplemental, or Furnace replacement
   backup. Conventional heating uses W and requires backup type None.
3. Set reversing-valve polarity, pre/post-change delays, and heating fan ownership
   according to the equipment requirements. Choose whether O/B retains its state
   during Off, temperature faults, and update shutdown.
4. Set the supplemental threshold and interval. Optionally enable automatic backup
   with Local demand, Outdoor temperature, or Either policy. Outdoor policies also
   require a balance point and **Outdoor balance point confirmed**.
5. Check temperature/humidity calibration, display units, and touch alignment.
6. Select **Confirm setup** (Home Assistant: **Confirm equipment setup**). This
   leaves the climate Off; select an operating mode afterward.

Equipment changes turn the climate Off and require confirmation again. Allow at
least 30 seconds for saved settings to reach flash before removing power.

### Operating settings

| Setting | Default / behavior |
| --- | --- |
| Compressor restart lockout | 5 minutes, including startup and direction changes |
| Normal minimum heating/cooling run | 3 minutes |
| Idle and backup-source transition interval | 30 seconds |
| Supplemental threshold / interval | 1°C or 5 minutes; requests repeat at the interval |
| Heat/cool deadband and overrun | 0.5°C |
| Minimum heat/cool target separation | 2°C |
| O/B pre-change / post-change delay | 30 seconds each; configurable 0–300 seconds |
| Retain O/B on stop | On; applies while powered, not across reboot or power loss |
| Display sleep | 30 seconds; touch or presence wakes it |

During a heating call, supplemental requests advance a two-stage heat pump to
Y2, then to backup when automatic backup is enabled. A single-stage heat pump
selects backup on the first request. During cooling, supplemental requests select
Y2. Staging resets when the call ends. Electric backup supplements compressor
heating; furnace backup runs with the compressor off. For emergency heat, enable
the Emergency heat switch and select Heat mode.

Temperature failure, explicit Off, and update shutdown stop HVAC outputs even
within minimum run times. A failed OTA attempt leaves outputs inhibited until
restart. Humidity failure alone does not stop temperature control.

### Outdoor temperature and calibration

The default outdoor source is Home Assistant entity
`sensor.smart_thermostat_outdoor_temperature`. Override `outdoor_entity` in your
local substitutions and set **Outdoor sensor units** to match its values.
Alternatively, point `outdoor_sensor_id` at an ESPHome sensor ID; see
[`examples/custom-outdoor.yaml`](examples/custom-outdoor.yaml).

Publish outdoor readings at least every 15 minutes. Missing or stale outdoor
data falls back to local-demand backup and exposes a diagnostic indication.

Start calibration offsets at zero and compare the assembled thermostat against
a reference. Temperature control and configuration entities use Celsius;
the touchscreen displays and adjusts values in the selected units. Home Assistant
handles its own display units. Setpoint steps are 1°F or 0.5°C.

## Verify hardware before connecting equipment

Confirm the physical terminal for every output on the disconnected board.
GPIO41 → Y2 has not been verified at the terminal.

| UWP terminal | GPIO | Function |
| --- | ---: | --- |
| W | 5 | Conventional heat |
| Y | 6 | Compressor stage 1 |
| Y2 | 41 | Compressor stage 2; verify terminal |
| G | 4 | Fan |
| O/B | 7 | Reversing valve |
| AUX/W2 | 39 | Heat-pump backup |

W and AUX/W2 are separate outputs. E is unsupported. R and C are power
connections. Conventional second-stage furnace heat is unsupported.

Before reconnecting equipment, check:

- Sensor readings remain valid through cold starts and extended operation;
  temperature failure stops calls and recovery respects restart lockouts.
- Touch works at all four corners; waking the display does not activate a control.
- Startup lockout, heating/cooling, fan Auto/On, staging, and source transitions
  match the configured system. Y2 never runs without Y1; furnace backup never
  overlaps the compressor. Check O/B timing and retention settings.
- Off, reboot, network loss, and updates behave as expected. Check backup policies,
  emergency heat, and stale outdoor data if those features are configured.

Complete supervised equipment testing before relying on the thermostat.

## Develop

| Path | Purpose |
| --- | --- |
| `smart-thermostat.yaml` | Factory entrypoint, substitutions, provisioning, adoption metadata |
| `packages/hardware.yaml` | Board pins, buses, display, touch, physical outputs |
| `packages/sensors.yaml` | Sensor filters, calibration, outdoor input, fault handling |
| `packages/climate.yaml` | Thermostat, equipment settings, supplemental requests |
| `packages/climate/outputs.yaml` | Output routing, interlocks, transition scripts |
| `packages/ui.yaml` | Shared LVGL theme, lifecycle, refresh scheduling, and touch feedback |
| `packages/ui/screens/` | Screen layouts, refresh actions, screen-specific scripts, and settings sections |
| `packages/ui/*.yaml` | Settings page templates with state synchronization, and shared navigation |
| `include/`, `components/` | Temperature UI helpers and generated setup-AP password |
| `tests/` | Configuration profiles, Python/helper tests, controller integration |
| `scripts/`, `installer/` | Release preparation, packaging, browser installer |

For HVAC changes, edit thermostat settings in `climate.yaml` and output routing
in `climate/outputs.yaml`. For UI changes, edit the relevant screen or settings
section. Screen files contain their layouts and behavior; settings templates bind
pages to their entities and contribute refresh actions. Shared styles, display
lifecycle, and refresh scheduling are in `ui.yaml`.

When changing hardware configuration, verify sensor readings and touch
calibration on the board. Keep display memory usage within the no-PSRAM target.

### Run checks

Activate the development environment above. Python tests and configuration checks:

```sh
python -m unittest discover -s esphome/tests -p 'test_*.py' -v
esphome config esphome/smart-thermostat.yaml
for profile in esphome/tests/*.yaml; do esphome config "$profile"; done
```

Compile and run C++ helpers with a C++20 compiler:

```sh
ESPHOME_INCLUDE=$(python -c 'import pathlib, esphome; print(pathlib.Path(esphome.__file__).parent.parent)')
g++ -std=c++20 -Wall -Wextra -Werror -I esphome/tests/host -isystem "$ESPHOME_INCLUDE" esphome/tests/test_helpers.cpp -o /tmp/test-helpers
/tmp/test-helpers
```

Run the controller integration tests on Linux with a host C++ compiler:

```sh
esphome compile esphome/tests/host/thermostat.yaml
THERMOSTAT_TEST_PREFS=$(mktemp -d)
ESPHOME_PREFDIR="$THERMOSTAT_TEST_PREFS" timeout 120s esphome/tests/host/.esphome/build/thermostat-test/.pioenvs/thermostat-test/program
```

Use a fresh preferences directory for each run. These tests use simulated sensors
and outputs. CI runs these checks and compiles all equipment profiles, encrypted
API, and custom outdoor sensor configurations. Test hardware-dependent changes
on the board before connecting HVAC equipment.

### Publish a release

Set the version and package import tag before committing a release:

```sh
python esphome/scripts/prepare_release.py esphome-v0.1.0
python esphome/scripts/prepare_release.py esphome-v0.1.0 --check
```

Use the next release version, review and commit the changes, then create and push
that immutable `esphome-v*` tag. Enable **Repository Settings → Pages → Source:
GitHub Actions** before the first release.

[The ESPHome workflow](../.github/workflows/esphome.yml) tests and builds the
configurations, publishes factory/OTA binaries, checksums, version metadata and
ESP Web Tools manifests, and deploys the installer. To upgrade an adopted source
configuration, update its package tag and rebuild.
