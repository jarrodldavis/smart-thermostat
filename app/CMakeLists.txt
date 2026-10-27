cmake_minimum_required(VERSION 3.16.0)
if(THERMOSTAT_MATTER)
    set(EXECUTABLE_COMPONENT_NAME src)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/matter")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.matter.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(THERMOSTAT_MATTER)
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.matter.lock")
endif()
project(espidf-test)

if(THERMOSTAT_MATTER)
    # PlatformIO uses SCons rather than Ninja for compilation. Materialize the
    # certificate assembly that esp_insights normally generates at build time.
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DDATA_FILE=${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_insights/server_certs/https_server.crt
            -DSOURCE_FILE=${CMAKE_BINARY_DIR}/https_server.crt.S
            -DFILE_TYPE=TEXT
            -P $ENV{IDF_PATH}/tools/cmake/scripts/data_file_embed_asm.cmake
        RESULT_VARIABLE embed_result)
    if(NOT embed_result EQUAL 0)
        message(FATAL_ERROR "Could not generate the ESP Insights certificate assembly")
    endif()
endif()
