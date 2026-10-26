# Matter build and provisioning verification

## Build integration

The `esp32s3-matter` PlatformIO environment uses Espressif32 6.10.0,
ESP-IDF 5.4.0, and the pinned `espressif/esp_matter` 1.4.2 managed component.
The default `esp32s3` environment continues to build MQTT and telnet.

Run inside the existing devcontainer:

```sh
cd /workspaces/smart-thermostat/app
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

At this build-integration boundary, configure Wi-Fi through standalone
provisioning and reboot before using Matter. The next layer adds combined
Matter/Wi-Fi commissioning and the commissioning touchscreen.

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
