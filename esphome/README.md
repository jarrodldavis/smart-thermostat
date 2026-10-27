# Smart Thermostat — ESPHome

An ESPHome implementation alongside the legacy ESP-IDF application in `../app`.
Project identity: **smeisner.smart-thermostat**. Targets the **V0.7.3 prototype,
September 16, 2026**, ESP32-S3-WROOM-1 N16, 16 MB flash, **no PSRAM**.
Requires **ESPHome 2026.10.0** and Python 3.12–3.14 (CI uses Python 3.13).

The factory configuration starts **uncommissioned**. Sensor readings and the
interface work, but HVAC outputs are inhibited until equipment setup is confirmed.
This implementation requires physical acceptance testing before installed use;
a successful compile does not establish that relay routing or I²C works on a board.

## Install and adopt

1. Record the old thermostat's equipment settings and back up its firmware/configuration.
2. Disconnect this board from the UWP/HVAC wiring. Use USB power and a data cable.
3. Install a tagged ESPHome factory release using the GitHub Pages installer,
   ESPHome Web, or `esphome run smart-thermostat.yaml --device /dev/ttyACM0`.
   Initial migration requires USB and erasure of incompatible legacy settings.
4. Provision Wi-Fi over physical USB using serial Improv, or open
   **Diagnostics → Wi-Fi setup** on the thermostat.
   Join the displayed `…-setup` network using the 20-character password shown on
   screen (concatenate both lines without spaces), then open `192.168.4.1` and
   enter the home Wi-Fi credentials. The fallback AP starts automatically when
   Wi-Fi is unavailable; allow about 90 seconds if credentials were previously saved.
5. Add the discovered ESPHome integration in Home Assistant **Settings → Devices & services**.
   This exposes entities; it does not create a Device Builder configuration.
6. Separately adopt the device in **ESPHome Device Builder**. The generated local
   configuration imports the public package from its immutable release tag.
   Set a unique name, local Wi-Fi credentials, and native API encryption there,
   then install the adopted configuration. `examples/adopted.yaml` illustrates it.
7. Complete commissioning below before operating installed equipment.

Factory packages contain no user secrets, shared setup password, shared API key,
or preset OTA password.
The owner adds API encryption and may add an OTA password in their local config.
Physical USB serial Improv and the password-protected native fallback AP provide
recovery without Home Assistant. BLE provisioning is not enabled. Loss of Wi-Fi or API connectivity does not reboot
or stop local HVAC control. An outdoor policy can fall back to local demand.

The public import URL becomes usable after the matching source tag is pushed.
For an unreleased working tree, use the local `smart-thermostat.yaml` entrypoint.
Do not expect adoption from an unpublished tag to succeed.


### Proof of possession for Wi-Fi setup

ESPHome's built-in `wifi.ap` and `captive_portal` manage setup, automatic fallback,
and saving home Wi-Fi credentials. A small `wifi_setup_password` helper generates
a fresh 20-character password at boot and assigns it to the native AP before
Wi-Fi initializes. It also runs in safe mode. Generation uses Espressif's
hardware entropy source before RF/ADC initialization; the base32 password has
100 bits of entropy and is not derived from the MAC address or stored in flash.

Open **Diagnostics → Wi-Fi setup** and scan the QR code with your phone's camera
or Wi-Fi QR scanner to join the WPA2-protected setup network. Then open
`http://192.168.4.1/` if the captive portal does not appear automatically. The SSID
and password remain visible for manual entry; enter both password lines without
spaces. The QR encodes the same generated credentials, with no external service.
The password stays the same for that boot, including repeated fallback activations,
and changes on reboot. Normal ESPHome logging is retained, including its AP
credential log.
The touchscreen is unavailable in safe mode; recovery can use saved home Wi-Fi
credentials or physical USB flashing.

ESPHome controls when fallback starts and stops; there is no custom radio
management, manual setup window, or session timer. Leaving the setup screen does
not stop the AP. Users of the shared factory binary obtain their individual
password from their device rather than from a shared published configuration.

Verify on hardware: the fallback network is secured; missing/wrong passwords fail;
the displayed password works; submitting home credentials connects the thermostat;
and rebooting changes the displayed password without losing the saved home Wi-Fi
credentials. Check startup and repeated fallback transitions for unexpected open
networks. These over-the-air checks still require the board.

## Equipment and terminals

| Driver | UWP terminal | GPIO | Function |
|---|---|---:|---|
| Q2 | W | 5 | Conventional furnace heat |
| Q3 | Y | 6 | Compressor stage 1 |
| Q4 | Y2 | **41** | Compressor stage 2, accepted provisional mapping |
| Q5 | G | 4 | Fan |
| Q6 | O/B | 7 | Heat-pump reversing valve |
| Q7 | AUX/W2 | 39 | Electric supplemental or replacement furnace heat |

The mappings combine firmware definitions, user trace observations, and a relay
click test. GPIO41 activated the additional relay; the specific Q4/Y2 terminal
assignment still needs verification on the disconnected board. Do not substitute
GPIO39 for Y2: the old firmware's `HVAC_STAGE2_PIN` refers to auxiliary heating.

**W and AUX/W2 are distinct.** The board does not support E. The upstream
maintainer instructed the user to move their furnace-backup wire from E to
AUX/W2. Emergency heat is a controller mode calling AUX/W2 with both compressor
stages off; it does not need an E output.

R and C are power connections, not GPIO control outputs. Single-stage W heat is
supported; conventional second-stage furnace heating is not. Up to two compressor
stages are supported for cooling, and for heating on a heat pump.

Other connections: I²C SDA36/SCL35; radar RX15/TX16/presence18; LDR8; buzzer17;
backlight14; status LEDs37/38/2; SPI CLK13/MOSI12/MISO21; display CS9/DC11/reset10;
touch CS47/IRQ48; SD CS40 held inactive. The display is a 3.2-inch ILI9341 with
XPT2046 touch, rendered at 320×240 using LVGL rotation and a 10% partial buffer.

## Commissioning

Use the touchscreen's **Settings** categories or the configuration entities.
Each category shows one setting at a time, with Previous/Next navigation, explicit
choice buttons, on/off controls, or plus/minus adjustment:

1. Select Conventional or Heat pump, and one or two compressor stages.
2. For heat pumps choose no backup, Electric supplemental, or Furnace replacement.
   Conventional equipment must use backup type None; it heats through W.
3. Set O/B polarity and its pre/post-change delays to the equipment requirements.
   Choose whether to retain O/B during Off, sensing faults and update shutdown.
   Select equipment-controlled or
   thermostat-controlled heating fan. Heat-pump compressor operation always calls G;
   furnace backup uses the configured furnace fan ownership.
4. If desired, enable automatic backup and select Local demand, Outdoor temperature,
   or Either. Outdoor policies require setting the balance point and explicitly
   enabling **Outdoor balance point confirmed**.
5. Set the native supplemental threshold and interval. Confirm sensor calibration
   and touchscreen orientation on the actual board.
6. Press **Confirm setup** on the Settings menu (or **Confirm equipment setup**
   in Home Assistant). Check the Equipment commissioned entity.
   Confirmation leaves the climate mode Off; select an operating mode afterward.

Equipment configuration changes immediately turn the climate off, invalidate
commissioning, and clear emergency heat. Confirmation is persisted; allow at least
30 seconds for preference writes before removing power. Emergency heat is not
restored after reboot. Its touchscreen On choice is disabled when unsupported; the
Home Assistant switch remains discoverable but cannot activate unsupported backup.

### Defaults and transitions

- Heat/cool deadband and overrun: 0.5°C; minimum target separation: 2°C.
- Native thermostat startup lockout: 5 minutes; minimum normal heating/cooling
  run time: 3 minutes; idle interval: 30 seconds. Ordinary authorized calls have
  no additional custom startup/idle delay. A shared five-minute lockout tracks
  actual compressor stops across heat/cool direction changes and backup interruptions;
  native heat/cool off timers are separate and cannot cover those physical transitions.
- Native supplemental actions fire when the deficit exceeds 1°C **or** the
  heating/cooling action reaches 5 minutes, then repeat at that runtime interval.
  These values are editable as Supplemental threshold and Supplemental interval.
  Timing belongs to the native climate action, not actual relay energization.
  Cooling requests select Y2 when two compressor stages are configured.
- Local backup uses the repeated native heating callbacks: on a two-stage heat
  pump, the first request selects Y2 and the second selects backup. On a
  single-stage heat pump, the first request selects backup. Later callbacks have
  no additional effect. Stage/backup requests reset at call end, equipment changes,
  faults, explicit off and shutdown. No separate backup deficit/delay timer remains;
  the old Backup demand threshold/delay controls and screens have been removed.
- Y2 requires Y1, two configured stages, a matching native supplemental request,
  and its own five-minute restart lockout after an actual stop. Conventional
  equipment stages cooling only. There is no separate stage-two minimum run or
  de-escalation timer; requests remain selected until the call ends.
- With automatic backup enabled, electric backup adds AUX alongside compressor
  heating. Furnace backup drops Y2/Y1, waits 30 seconds, then enables AUX.
  Return transitions retain the all-off interval and shared compressor lockout.
- Outdoor backup: at/below the configured balance point. Either policy uses the
  outdoor or local condition. Backup stays selected until the heating call ends.
- Emergency heat disables compressor operation immediately; it supplies backup
  only during a heating call. Select Heat mode when using emergency heating.
- Off, temperature faults, update shutdown, and emergency/source transitions
  override minimum run timers. Heat sources and fan are inhibited; O/B follows
  the configured retention policy.
- Fan Auto follows equipment calls. Fan On follows native fan-mode callbacks and
  their switching timer while the climate is enabled. Fan-only mode is available. There is no rolling-hour circulation.

HVAC operation is native ESPHome YAML in `packages/climate.yaml` and
`packages/climate/outputs.yaml`. There is no custom HVAC C++ controller, input/
output snapshot layer, timer arithmetic, or background control poller.

Thermostat primary actions clear native request switches and apply the selected
equipment configuration. Repeated supplemental actions select Y2 and then backup
using native `if`, `select.is`, and `switch` actions. Internal request switches
are never restored. Native output switches expose actual relay state to fan
control, stage diagnostics and physical interlocks; the hardware package retains
the pin mapping. Off, faults, uncommissioned settings and shutdown stop pending
start scripts before turning off outputs.

Native restartable scripts implement compressor/Y2 cooldowns and valve/backup
settling with `delay`; starts use `wait_until`, then recheck commissioning,
sensing and current thermostat intent. Normal startup, hysteresis, minimum run,
idle and supplemental timing remain in the native thermostat. There are no
parallel custom timers. The shared `compressor_off_time`, `stage2_off_time` and
`source_settle_time` substitutions default to 5 minutes, 5 minutes and 30 seconds;
the last applies to backup-source transitions, independently of the O/B controls.

O/B configuration is available in Home Assistant/Device Builder and under
**Settings → Equipment → Reversing valve** on the touchscreen:

| Control | Initial default | Range / behavior |
| --- | --- | --- |
| Reversing valve pre-change delay | 30 seconds | 0–300 seconds; minimum off interval before a valve command |
| Reversing valve post-change delay | 30 seconds | 0–300 seconds; command-to-compressor-start interval |
| Retain reversing valve on stop | On | Keep the current command during Off, faults and update shutdown |

The delay controls use seconds and do not change with display temperature units.
Changing any of these equipment settings turns the climate Off and requires
recommissioning. These defaults are commissioning starting points, not universal
manufacturer timings. Zero disables the corresponding delay, without disabling
compressor/Y2 cooldowns or the requirement to stop Y2/Y1 before moving O/B.

The pre-change script starts at an actual Y1 falling edge, and once at startup
because the last physical stop time is unknown. Changing its setting also
restarts the guard conservatively. A request made after
that interval has elapsed does not start a fresh pre-delay. Only a real O/B state
change starts the post-change script. Compressor startup waits for the valve
command and post-delay as well as its existing restart lockouts; overlapping
waits run concurrently. Off/fault/update cancellation cannot leave a pending
energization or compressor start behind. With retention disabled, O/B is
commanded off through the same pre-change gate. Normal satisfied-call idle
retains O/B regardless of this option.

Retention is a powered-runtime policy, including the update-shutdown hook. The
O/B relay is not restored from flash, and power loss, GPIO reset or reboot at the
end of an OTA update can still deenergize it. Keeping the physical valve command
uninterrupted across those events would require hardware/reset behavior outside
this native-script policy. A timed post-change wait is not physical valve-position
feedback.

Small expressions remain where native YAML has no equivalent: reading the
climate action enum, validating a finite sensor sample, comparing a configurable
outdoor balance point with unit conversion, and applying live staging settings
through the thermostat's public setters. Equipment choice, output writes, stage
selection, delays and cancellation use native actions and conditions.

## Sensors and outdoor temperature

AHT20 sampling is every 10 seconds. Raw temperature is validated before native
exponential smoothing (weight 0.1 per new sample), then receives an adjustable
native `offset` and a `timeout: 60s` filter. Invalid raw readings become `NaN`
immediately and do not contaminate the smoothing history. A missing update also
becomes `NaN` after 60 seconds. Before the first reading the sensor is invalid.
HVAC validity reads this filtered state directly; there are no freshness globals
or timestamp bookkeeping. Sensor-fault shutdown still overrides minimum run timers.

Humidity uses native smoothing, offset and a 60-second timeout, retaining a small
NaN-preserving bounds check. Invalid humidity displays as unavailable and does
not independently disable temperature control.
Calibration is applied once. Valid readings permit recovery subject to compressor
lockout.

Control, persisted targets, calibration and Home Assistant configuration-number
entities use Celsius. The touchscreen converts every temperature setting to the
selected Celsius/Fahrenheit units, including the balance point, calibration offset,
and staging/backup thresholds. Balance point is an absolute temperature; offsets
and thresholds are temperature differences (multiply by 1.8 without adding 32).
Setpoint buttons use whole 1°F or 0.5°C steps. Fahrenheit balance-point adjustment
uses 1°F steps; offsets and thresholds use 0.1°F steps to preserve useful precision.
Switching display units does not reinterpret or rewrite the stored temperatures.
Start calibration at zero and measure the assembled board. The legacy firmware's
−5.3°F temperature and +13 percentage-point humidity offsets are historical values,
not calibration for every board. Humidity is clamped to 0–100% only for valid samples.

Native I²C is configured at 50 kHz. The old application used software I²C;
verify repeated reads, cold starts, and disconnection recovery on the physical PCB.
A custom bus workaround is deliberately not introduced without evidence it is needed.

By default, the internal Home Assistant sensor subscribes to
`sensor.smart_thermostat_outdoor_temperature`. Override `outdoor_entity` with your
entity, and configure **Outdoor sensor units** to match its numeric values.
Home Assistant reports a sensor's configured units; do not assume Celsius.
Re-publish outdoor readings regularly: after 15 minutes without a valid update,
outdoor policies fall back to local demand and report Outdoor policy fallback.

Alternatively set `outdoor_sensor_id` to your own ESPHome sensor. A native `copy`
sensor subscribes to every published value automatically and applies `timeout:
15min` to its private control state. No per-sample automation is needed; see
`examples/custom-outdoor.yaml`. The original source sensor is not modified.
Outdoor unit selection is applied when reading the control copy, so changing the
unit setting does not require waiting for another sample.
Balance point confirmation is required for commissioning an enabled outdoor policy.

## Display and diagnostics

Main screen: current temperature/humidity, separate heat/cool targets, mode,
automatic/continuous fan, source and compressor stage. Settings includes equipment,
backup, calibration, units and display preferences. Diagnostics reports network,
commissioning and faults. Presence or touch wakes the display; a waking touch is
consumed before any widget is activated. Sleep defaults to 30 seconds and is
suppressed while presence remains detected. The display starts at 70% brightness;
automatic screen brightness is opt-in in Settings and follows the LDR with a 20%
minimum. Backlight and buzzer use separate PWM timers. Optional touch beeps apply
to every accepted button click, including Settings and navigation; waking a sleeping
screen alone does not activate a button or beep.
Touch calibration values are starting estimates: validate all
four corners and adjust XPT2046 calibration/transform locally if necessary.
The portrait touch Y axis is mirrored to match the display's 90-degree rotation.
The interface uses ESPHome’s built-in LVGL dark theme, native **GRID layouts**, shared styles, and
**buttonmatrix** widgets for mode/fan choices, settings options, navigation and
plus/minus controls. Matrices reduce memory use while retaining touch targets at
least 40 px high and 55 px wide. All interaction uses taps; there are no scrolling
lists, dropdowns or sliders. Unstyled `container` widgets group labels without
overriding inherited text styling. Button spacing lives in the shared widget
theme; scroll flags remain explicit interaction settings. Larger/centered labels
use shared role styles. The QR canvas has an explicit white quiet zone for scanning.

Settings has Equipment, Backup heat, Staging, Display and Calibration categories.
Each setting has its own explicit entity binding and Previous/Settings/Next
footer. Settings pages are grouped by section in `packages/ui/screens/`: Equipment,
Backup heat, Staging, Display and Calibration. Other screens have their own files;
`packages/ui.yaml` retains shared styling, lifecycle hooks, scripts and refresh
actions. The small YAML templates in `packages/ui/` share layouts using ESPHome's
include variables; there is no runtime entity-pointer dispatcher. Choices call
native `select.set`, `switch.turn_on/off`, `number.set` and `climate.control`
actions. Selection highlights refresh from entity state, including changes from
Home Assistant. Emergency heat On is disabled unless a heat pump and backup are
configured. Lambdas format values and convert temperature units; Fahrenheit
setpoints still advance in whole degrees, with offsets/differences handled
separately from absolute temperatures. Normal display sleep and first-touch wake
behavior remain in effect.

Display sleep uses the native `lvgl.is_idle` condition with the configured timeout.
A one-second check also requires both presence inputs to be off. Rechecking avoids
missing sleep when presence clears after the idle threshold was crossed. Native
`on_pause` turns the backlight off and `on_resume` applies the selected brightness.
Touch release and presence call `lvgl.resume`, which resets LVGL activity and
redraws without a separate timer or redraw action. Ambient brightness still
updates while awake. The waking touch remains ignored by the paused LVGL input
listener until release, preserving first-touch wake behavior.

Touch uses the native `xpt2046` component at 1 MHz with **GPIO48 PENIRQ**, a
single pressure threshold of **600**, and `touch_timeout: 80ms` for release
rechecks. IRQ operation, held presses, releases and on-screen controls were
verified on the prototype. The threshold rejects measured idle noise up to 496;
there is no custom touch driver or recurring polling fallback.

A one-time startup workaround in `packages/hardware.yaml` sends `0xD0,0,0` inside
an explicitly selected SPI transaction at boot priority 500, after native touch
setup at 600. ESPHome 2026.10.0's native initialization sends that command without
asserting CS, so it can leave PENIRQ disabled. The corrected transaction resolved
the board's interrupt-mode failure. The command sets PD1/PD0=00 to power down the
ADC and enable PENIRQ. See the [native driver source](https://api-docs.esphome.io/xpt2046_8cpp_source)
and [XPT2046 datasheet](https://files.waveshare.com/wiki/common/XPT2046_Datasheet.pdf).
Retain the workaround until the upstream initialization is corrected and tested.

Temporary GPIO48 monitoring, custom per-touch logs and IRQ/polling comparison entrypoints
have been removed. The late LVGL/equipment boot hooks remain separate at -100,
and first-touch wake behavior is preserved. For hardware acceptance, check idle
stability, all four corners, tap/hold/release, and waking without activating a
control. `touch_timeout` is a release recheck interval, not pressure hysteresis
or debounce.


OTA start and shutdown inhibit outputs. If OTA fails, outputs remain inhibited
until restart; do not silently resume after an interrupted update.

## Build, test and publish

```sh
python3.13 -m venv /tmp/smart-thermostat-venv
. /tmp/smart-thermostat-venv/bin/activate
pip install esphome==2026.10.0
esphome config esphome/smart-thermostat.yaml
esphome compile esphome/smart-thermostat.yaml
ESPHOME_INCLUDE=$(python -c 'import pathlib, esphome; print(pathlib.Path(esphome.__file__).parent.parent)')
g++ -std=c++20 -Wall -Wextra -Werror -I esphome/tests/host -isystem "$ESPHOME_INCLUDE" esphome/tests/test_helpers.cpp -o /tmp/test-helpers
/tmp/test-helpers
python -m unittest discover -s esphome/tests -p 'test_*.py' -v
esphome compile esphome/tests/host/thermostat.yaml
NATIVE_TEST_PREFS=$(mktemp -d)
ESPHOME_PREFDIR="$NATIVE_TEST_PREFS" timeout 120s esphome/tests/host/.esphome/build/native-thermostat-test/.pioenvs/native-thermostat-test/program
```

CI validates and compiles the factory, eight equipment profiles, encrypted API,
and a custom outdoor sensor fixture. The host fixture runs the real ESPHome
thermostat with the production YAML automations and native template sensors/
outputs replacing physical hardware. It covers startup and minimum run timing,
repeated supplemental actions, fan callbacks, fault/Off inhibition, and native
script waits and cancellation. Equipment scenarios exercise conventional heat
and cooling, one/two compressor stages, electric/furnace/no backup, emergency
transitions, outdoor/Either/local policies and stale outdoor fallback. Every
physical output write is checked for Y2/Y1 ordering, W exclusion, furnace/
compressor exclusion, pre/post valve timing, and valve movement with the compressor
off. Additional cases cover powered Off/fault/update retention, disabled retention,
zero delays, and cancellation during both valve-change phases.

The separate C++ helper tests cover display conversions, unit-based stepping,
target bounds and password generation. Conversion tests use installed ESPHome
helpers rather than a second implementation. The fixture's shortened timing
substitutions do not change production defaults. Physical IRQ, output terminals,
loads, sensor reliability and supervised HVAC acceptance still require the board.
Packaging tests check the official manifest, firmware selection and checksums.

Release preparation **must happen before committing/tagging** so source import
metadata matches factory firmware:

```sh
python esphome/scripts/prepare_release.py esphome-v0.1.0
# Review and commit the version change, then create/push that immutable tag.
```

The separate ESPHome workflow handles `esphome-v*` tags, preserving legacy `v*`
releases. It checks metadata, builds all variants, attaches factory/OTA images,
the official Web Tools manifest, SHA256 checksums and version metadata to a GitHub release,
and deploys the installer to GitHub Pages. Enable **Pages → Source: GitHub Actions**
in repository settings before the first release. The installer and all firmware
assets are published together over HTTPS. The pinned official `esphome/build-action`
builds factory firmware and generates the Web Tools manifest from ESPHome artifacts.
Our packaging script preserves that manifest and its factory/OTA filenames, adding
only project metadata, checksums and installer customization. Debug ELF files and
unrelated local files are not published. The browser installer offers erasure as
an advanced option; select it when migrating from legacy firmware.

Changing a tag is unsupported. Publish a new version and explicitly update the
adopted package ref to upgrade. No automatic unreviewed package updates occur.

## Physical acceptance checklist

These checks require the board and are not replaced by compilation or host tests:

- Disconnected-load verification of W/Y1/Y2/G/O-B/AUX terminals and active polarity,
  particularly GPIO41→Q4/Y2. Verify E remains unused.
- Cold-start defaults, power interruptions, output de-energization and OTA behavior.
- Reliable AHT20 readings/fault recovery, radar UART, ambient light, touch corners,
  display rotation, shared SPI and display responsiveness.
- Available heap under API connections, LVGL updates and OTA on the no-PSRAM board.
- Timed stage transitions, reversing-valve transitions, backup takeover and the
  absence of compressor/furnace overlap; supervised tests with installed equipment.
- Browser USB install, serial/protected-AP provisioning, HA discovery, Device Builder adoption,
  encrypted API, OTA and retained commissioning settings.

Do not mark hardware support verified until these checks pass. USB can restore
legacy firmware, but back up settings first: legacy NVS import and cross-firmware
OTA compatibility are not provided.

Deferred: Matter, SD logging/core dumps, legacy MQTT compatibility, second-stage
conventional furnace heat, rolling-hour circulation and advanced weather/window
automation. Local reference photos in `assets/codex` stay ignored and are never
included in installer or release assets.
