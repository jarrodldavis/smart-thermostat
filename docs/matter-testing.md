# Matter build and provisioning verification

## Build integration

The `esp32s3-matter` PlatformIO environment uses Espressif32 6.10.0,
ESP-IDF 5.4.0, and the pinned `espressif/esp_matter` 1.4.2 managed component.
The default `esp32s3` environment continues to build MQTT and telnet.

With PlatformIO installed, run from the repository root:

```sh
cd app
pio run -e esp32s3
pio run -e esp32s3-matter
```

The Matter profile retains SoftAP and IPv6, selects example attestation
credentials, limits unrelated clusters, and reserves the original 8 KB control
task stack before network allocations. Its memory settings target operation
with external PSRAM disabled; this describes the configuration, not a verified
absence of PSRAM hardware. The default partition layout is shared by both builds.

The original identity is retained: vendor `Tah Der`, product `Smart prod`,
hardware `v2.0`, and node label `RealSmartThermostat`. The thermostat endpoint
retains its original optional limits and PI demands. Values remain test attributes;
Matter writes do not control HVAC relays.

The commissioning layer below replaces separate first-time Wi-Fi setup with
combined Matter/Wi-Fi commissioning and the commissioning touchscreen.

## Upload and capture

Build first. Start the UART monitor in one terminal before uploading in another:

```sh
pio device monitor -e esp32s3-matter --port /dev/ttyUSB0
```

```sh
pio run -e esp32s3-matter -t upload --upload-port /dev/ttyACM0
```

Use the board's actual ports. The upload preserves configuration. For a clean
first-time test, erase and then perform the full upload; erasing clears Wi-Fi,
thermostat settings, and Matter pairings, including the bootloader/table:

```sh
pio run -e esp32s3-matter -t erase --upload-port /dev/ttyACM0
pio run -e esp32s3-matter -t upload --upload-port /dev/ttyACM0
```

An app-only upload is insufficient after erase. The Matter SDK configuration is
generated as `sdkconfig.esp32s3-matter` from `sdkconfig.defaults` followed by
`sdkconfig.matter.defaults`. Remove only that generated Matter configuration
and rebuild when testing changed defaults. Managed downloads live in
`managed_components/`; the committed lock pins their versions.

Use `pio run -e esp32s3-matter -t menuconfig` to edit Matter settings, then
**D — Save minimal config** to export them for review. The export includes the
combined configuration; copy Matter-specific changes into `sdkconfig.matter.defaults`
and shared changes into `sdkconfig.defaults`.

BLE commissioning and QR rendering are opt-in. To enable Matter over BLE, use
`menuconfig` to enable Bluetooth, the NimBLE host, and CHIP-over-BLE, then record
those choices in `sdkconfig.matter.defaults`. To enable rendered QR codes, add
`-D LV_USE_QRCODE=1` to the environment's `build_flags` in `platformio.ini`.
With the default configuration, use SoftAP to set up Wi-Fi and the manual Matter
pairing code to commission over the network.

The setup passcode is **20202021**, discriminator **3840**. Example attestation
credentials can produce a development/certification warning in controllers.

## SDK diagnostics

The pinned SDK emits duplicate `SEC_CERT_DAC_PROVIDER` choice warnings. Example
DAC selection remains configured. Its initial Breadcrumb write can also report
unsupported attribute before dynamic endpoint 0 is registered. These existing
SDK messages are distinct from allocation failures or commissioning failures.
No dependency patches are used.

The SDK may log an initial `ESP_ERR_WIFI_CONN` when a second connection request
arrives while the station is already connecting; verify the subsequent IP event.
The SD-card initialization error is separate from provisioning.

The integration follows Espressif's
[managed-component instructions](https://docs.espressif.com/projects/esp-matter/en/release-v1.4/esp32/developing.html#esp-matter-component-experimental).

## Combined commissioning

With the default configuration and no saved SSID, boot starts SoftAP Wi-Fi
provisioning and displays its service name, AP password, and PoP. Provision Wi-Fi
using the Espressif app's manual setup flow. Matter then starts in the same boot
and opens a network commissioning window; enter its manual code in Apple Home
or another Matter controller to add it.

When CHIP-over-BLE is explicitly enabled, boot can instead provision Wi-Fi and
add a fabric together through BLE commissioning. QR rendering is independently
optional; without it, enter the displayed manual code. Commissioning windows
last five minutes, the display stays awake, and the UI returns to the thermostat
after success.

Matter defaults to enabled in Matter builds, but a saved disabled setting wins.
Non-Matter builds and explicitly disabled Matter builds retain standalone Wi-Fi
provisioning. BLE is used only when explicitly enabled; otherwise they use SoftAP.

For an uncommissioned device already on Wi-Fi, Info → QR offers Network and
Stop, plus BLE when compiled in. Network is disabled until Wi-Fi connects. Codes carry the selected
rendezvous flags, and either transport opens a five-minute window. Already paired
devices hide the initial setup codes and use the controller's sharing flow.
A remaining fabric still counts as paired; fabric-management UX is outside scope.

After successful commissioning and BLE memory release when BLE is included, an 8 KB
background task saves the accepted driver credentials into application NVS,
discovers the timezone, and starts SNTP. Failed or incomplete commissioning does
not replace saved application credentials. TLS and NVS work stay off the CHIP
event task and wait for reclaimed heap. Valid synchronized time is preserved
across the SDK's startup clock reset.

### BLE ownership and cleanup

BLE is available for one owner per boot. A shared atomic claim lets standalone
provisioning or Matter reserve it before initializing its host. The saved Matter
Enable setting alone does not reserve BLE. Teardown never makes released memory
available for another BLE use in the same boot.

When CHIP-over-BLE is enabled, the SDK initializes Matter's BLE host even with
automatic advertising disabled. Builds without CHIP-over-BLE leave it unclaimed.
Stop, window timeout, selecting Network, and successful commissioning release
Matter BLE when no commissioning session still needs it. Paired startup also
reclaims BLE. Network commissioning remains available after reclamation when
Wi-Fi is connected. Reopening an active BLE window is idempotent.

Standalone Wi-Fi BLE provisioning likewise releases host/controller memory.
Its panel shows BLE, disabled BLE active, or disabled Restart for BLE according
to state. BLE claimed by Matter stays hidden on that standalone panel. Active
SoftAP status describes its own session; the BLE restart instruction stays on
the BLE button. Enabling Matter after standalone BLE provisioning saves the
setting and performs a normal restart when CHIP-over-BLE needs fresh BLE memory.
Matter without CHIP-over-BLE can start without that restart.

When only SoftAP is available, opening the Wi-Fi provisioning panel starts it
automatically. Stop ends the session; Close also stops it before dismissal.
Both restore the saved station configuration after cancellation or failure.
Non-Matter recovery reconnects the existing driver in place, preventing an old
blocking reconnect task from timing out during a rapidly opened next session.

### Commissioning checks

1. Erase and fully upload default Matter firmware; provision Wi-Fi through SoftAP
   using the displayed credentials. Confirm Matter starts without rebooting and
   commission over the network using the displayed manual pairing code.
2. Check successful fabric commitment, Wi-Fi IP, saved
   credentials, timezone/SNTP, HTTP, and controller reads/writes. Reboot and verify
   recovery without returning to setup.
3. With BLE enabled, test fresh combined Wi-Fi/Matter commissioning and memory
   reclamation. Before commissioning, exercise BLE → Stop and timeout. Confirm memory release
   and Restart for BLE, then use Network without rebooting where Wi-Fi exists.
4. Exercise standalone SoftAP Stop, Close, timeout, and incorrect-password recovery
   while Matter runs. Confirm restoration of the original network.
5. Repeat standalone provisioning on `esp32s3`: default SoftAP setup, cancellation,
   bad-password recovery, and rapid reopening. With BLE enabled, check initial BLE
   success and its post-session indicator. With QR enabled, check scannable codes.

Preserve logs from before boot through each case. Compile checks do not establish
on-device memory headroom. The final layer adds controller-read and runtime fixes
needed for the complete validation described below.
