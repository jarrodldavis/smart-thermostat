# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
  INCLUDE_DIRS "../include"
  REQUIRES wifi_provisioning protocomm bt esp_wifi esp_event esp_netif nvs_flash)
