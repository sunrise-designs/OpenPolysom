cmake_minimum_required(VERSION 3.16)

project(ProtoSom)

# --- Python virtual environment ---
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VENV_DIR "${CMAKE_SOURCE_DIR}/.venv")

if(WIN32)
    set(VENV_PIP "${VENV_DIR}/Scripts/pip.exe")
else()
    set(VENV_PIP "${VENV_DIR}/bin/pip")
endif()

add_custom_command(
    OUTPUT  "${VENV_DIR}/.stamp"
    COMMAND ${Python3_EXECUTABLE} -m venv "${VENV_DIR}"
    COMMAND ${VENV_PIP} install --upgrade pip setuptools wheel
    COMMAND ${VENV_PIP} install -r "${CMAKE_SOURCE_DIR}/python/requirements.txt"
    COMMAND ${CMAKE_COMMAND} -E touch "${VENV_DIR}/.stamp"
    DEPENDS "${CMAKE_SOURCE_DIR}/python/requirements.txt"
    COMMENT "Setting up Python venv and installing dependencies"
)

add_custom_target(python_venv DEPENDS "${VENV_DIR}/.stamp")
# -----------------------------------

set(POLAR_HR_SOCKET_PATH "/tmp/polar_hr.sock" CACHE STRING "Unix socket path for Polar HR data")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get git hash
execute_process(
    COMMAND git rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT GIT_HASH)
    set(GIT_HASH "unknown")
endif()

add_executable(main
    src/main.cpp
    src/patient.cpp
    src/hr_receiver.cpp
    src/sdp800.cpp
    src/Seeed_LDC1612.cpp
    src/edflib.c
)

target_compile_options(main PRIVATE -Wall -Wextra -pthread)
target_compile_definitions(main PRIVATE
    GIT_COMMIT_HASH="${GIT_HASH}"
    POLAR_HR_SOCKET_PATH="${POLAR_HR_SOCKET_PATH}"
)

# Special compile definitions for edflib.c
set_source_files_properties(src/edflib.c PROPERTIES
    COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE;_LARGEFILE_SOURCE"
)

target_link_libraries(main m pthread)