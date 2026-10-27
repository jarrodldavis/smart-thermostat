# SPDX-License-Identifier: GPL-3.0-only
"""Assign a device-generated password to ESPHome's native fallback AP."""
from esphome import codegen as cg, config_validation as cv
from esphome.core import CoroPriority, coroutine_with_priority

DEPENDENCIES = ['wifi', 'captive_portal', 'esp32']
ns = cg.esphome_ns.namespace('wifi_setup_password')
WiFiSetupPassword = ns.class_('WiFiSetupPassword', cg.Component)
CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(WiFiSetupPassword)}).extend(cv.COMPONENT_SCHEMA)


# Register before the safe-mode early return, so recovery never uses an empty AP
# password. Runtime setup runs even earlier, before ADC and Wi-Fi initialization.
@coroutine_with_priority(CoroPriority.COMMUNICATION)
async def to_code(config):
    var = cg.new_Pvariable(config['id'])
    await cg.register_component(var, config)
