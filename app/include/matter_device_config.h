// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Apply the original device identity to SDK translation units too, via
// CHIP_PROJECT_CONFIG_INCLUDE. Numeric VID/PID retain the SDK's test values
// to match the example attestation credentials used by this test build.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "Tah Der"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "Smart prod"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "v2.0"
