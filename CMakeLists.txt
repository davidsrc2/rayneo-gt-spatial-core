cmake_minimum_required(VERSION 3.21)

# Apple Silicon defaults; must be set before project().
if(APPLE)
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures")
  endif()
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
  endif()
endif()

project(rayneo_gt_spatial_core VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RAYNEO_BUILD_HAL     "Build the hidapi hardware layer and engine" ON)
option(RAYNEO_BUILD_TESTS   "Build core unit tests" ON)
option(RAYNEO_BUILD_APPS    "Build CLI tools" ON)
option(RAYNEO_FETCH_HIDAPI  "Fetch and build hidapi if not found on the system" ON)
# Homebrew's dylib targets the host macOS, not CMAKE_OSX_DEPLOYMENT_TARGET (ld warning,
# and the app would not run on older systems). Build hidapi statically by default on Apple.
if(APPLE)
  set(_rayneo_system_hidapi_default OFF)
else()
  set(_rayneo_system_hidapi_default ON)
endif()
option(RAYNEO_SYSTEM_HIDAPI "Use an installed hidapi instead of building it" ${_rayneo_system_hidapi_default})

# Per-target settings for our own code (never applied to third-party deps such as hidapi,
# which needs the full <windows.h>).
function(rayneo_warnings target)
  if(WIN32)
    # <windows.h> (hal) must not define min/max macros over std::min/std::max.
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core: protocol parsing + sensor fusion. Pure C++17, zero OS dependencies.
# ---------------------------------------------------------------------------
add_library(rayneo_core STATIC
  src/core/gt_protocol.cpp
  src/core/madgwick.cpp
  src/core/gyro_bias_estimator.cpp
  src/core/imu_pipeline.cpp
  src/core/tick_monitor.cpp
  src/core/head_calibration.cpp
)
add_library(rayneo::core ALIAS rayneo_core)
target_include_directories(rayneo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
rayneo_warnings(rayneo_core)

if(RAYNEO_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE rayneo::core)
  rayneo_warnings(core_tests)
  add_test(NAME core_tests COMMAND core_tests)
endif()

if(NOT RAYNEO_BUILD_HAL)
  return()
endif()

# ---------------------------------------------------------------------------
# hidapi: system package (RAYNEO_SYSTEM_HIDAPI) -> fallback to static FetchContent.
# ---------------------------------------------------------------------------
if(RAYNEO_SYSTEM_HIDAPI)
  find_package(hidapi 0.12 CONFIG QUIET)
endif()
if(TARGET hidapi::hidapi)
  set(RAYNEO_HIDAPI_TARGET hidapi::hidapi)
elseif(TARGET hidapi::darwin)
  set(RAYNEO_HIDAPI_TARGET hidapi::darwin)
else()
  if(RAYNEO_SYSTEM_HIDAPI)
    find_package(PkgConfig QUIET)
  endif()
  if(PkgConfig_FOUND)
    if(APPLE OR WIN32)
      pkg_check_modules(HIDAPI IMPORTED_TARGET hidapi)
    else()
      pkg_check_modules(HIDAPI IMPORTED_TARGET hidapi-hidraw)
    endif()
  endif()
  if(HIDAPI_FOUND)
    set(RAYNEO_HIDAPI_TARGET PkgConfig::HIDAPI)
  elseif(RAYNEO_FETCH_HIDAPI)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(hidapi
      GIT_REPOSITORY https://github.com/libusb/hidapi.git
      GIT_TAG        hidapi-0.15.0
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(hidapi)
    set(RAYNEO_HIDAPI_TARGET hidapi::hidapi)
  else()
    message(FATAL_ERROR "hidapi not found. 'brew install hidapi' or -DRAYNEO_FETCH_HIDAPI=ON")
  endif()
endif()
message(STATUS "rayneo: hidapi target = ${RAYNEO_HIDAPI_TARGET}")

# ---------------------------------------------------------------------------
# HAL: USB HID transport + high-frequency reader thread.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(rayneo_hal STATIC
  src/hal/hidapi_transport.cpp
  src/hal/imu_stream_reader.cpp
)
add_library(rayneo::hal ALIAS rayneo_hal)
target_link_libraries(rayneo_hal
  PUBLIC  rayneo::core Threads::Threads
  PRIVATE ${RAYNEO_HIDAPI_TARGET})
rayneo_warnings(rayneo_hal)

if(APPLE)
  # hidapi's darwin backend sits on IOHIDManager; link explicitly for static builds.
  target_link_libraries(rayneo_hal PRIVATE
    "-framework IOKit"
    "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------------------
# Platform: C ABI engine (portable) + macOS Objective-C++ facade.
# ---------------------------------------------------------------------------
add_library(rayneo_engine STATIC src/platform/rayneo_engine.cpp)
add_library(rayneo::engine ALIAS rayneo_engine)
target_link_libraries(rayneo_engine PUBLIC rayneo::hal)
rayneo_warnings(rayneo_engine)

if(APPLE)
  add_library(rayneo_macos STATIC src/platform/macos/RNSpatialEngine.mm)
  target_include_directories(rayneo_macos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos)
  target_compile_options(rayneo_macos PRIVATE -fobjc-arc)
  target_link_libraries(rayneo_macos PUBLIC rayneo::engine "-framework Foundation")
endif()

if(RAYNEO_BUILD_APPS)
  add_executable(imu_probe apps/imu_probe/main.cpp)
  target_link_libraries(imu_probe PRIVATE rayneo::engine)
  rayneo_warnings(imu_probe)
endif()
