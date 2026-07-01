cmake_minimum_required(VERSION 3.16)

# sdkconfig.local (gitignored) holds real Wi-Fi credentials and overlays
# sdkconfig.defaults; it's optional so a fresh checkout still configures.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};sdkconfig.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(polysom)
