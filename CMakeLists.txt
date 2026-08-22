# CMakeLists.txt — Temporal Forge Player
# spec 01: C++23, CMake + Ninja, Vulkan, FFmpeg, Qt6/QML, Linux first.
cmake_minimum_required(VERSION 3.24)

project(temporal_forge_player
    VERSION 0.1.0
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_definitions(TFORGE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Qt + RHI prefer Ninja for faster, more deterministic builds.
if(NINJA_EXE AND NOT EXISTS "${NINJA_EXE}")
    unset(NINJA_EXE CACHE)
endif()
find_program(NINJA_EXE ninja REQUIRED)

# --- dependencies ---
find_package(PkgConfig REQUIRED)

# FFmpeg (spec: libavformat/libavcodec/libavutil/libswscale/libswresample)
pkg_check_modules(FFMPEG REQUIRED
    libavcodec libavformat libavutil libswscale libswresample)

# Vulkan
# The loader's pkg-config file is useful for diagnostics, but it is not the
# source of truth for the headers.  Some systems ship the loader/ICD without
# the SDK development package, and stale CMake caches can also retain a
# `Vulkan_INCLUDE_DIR-NOTFOUND` value.
pkg_check_modules(VULKAN QUIET vulkan)
# Some runtime-only Vulkan installations provide the loader and ICD but not
# the development headers.  This repository carries the matching public
# headers needed to compile the player, so use them when CMake cannot find a
# system Vulkan include directory (including a stale NOTFOUND cache entry).
set(TFORGE_BUNDLED_VULKAN_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/vulkan_include")
if(EXISTS "${TFORGE_BUNDLED_VULKAN_INCLUDE}/vulkan/vulkan.h")
  set(Vulkan_INCLUDE_DIR "${TFORGE_BUNDLED_VULKAN_INCLUDE}" CACHE PATH
      "Vulkan include directory" FORCE)
endif()
find_package(Vulkan REQUIRED)

# FSR4 GLSL->SPIR-V shader compilation + embedding.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderCompile.cmake)
tforge_compile_shaders(
    OUT_VAR FSR4_SHADER_FILES
    OUT_DIR ${CMAKE_BINARY_DIR}/shaders/fsr4
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/prepass_pq_eotf.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/yuv_to_fsr_input.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/drm_yuv_to_fsr_input.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/codec_motion_expand.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_dw_dot4.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_pw_dot4.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_pw_coop.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_pw_fp16_direct.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_spatial_fp16_direct.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/residual_block_16_int8_coop.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_upscale_fp16_coop.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_upscale_fp16_scalar_final.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_downscale_fp16_coop.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_downscale_fp16_direct.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/conv_fp16_coop.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/residual_add.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/scatter.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/postpass_composite.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/downscale_rgb10.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/easu.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/bicubic_prefilter.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr4/spd.comp)

# Qt6 (spec: Core/Gui/Quick/Qml/Widgets)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Quick Qml Widgets ShaderTools)

# --- Qt deployment helpers ---
qt_standard_project_setup(REQUIRES 6.6)

# --- compile options (warnings but not pedantic; external/ is exempt) ---
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
)

# --- gather sources (exclude external/ — the FidelityFX SDK has its own CMake) ---
# Split into a static library (everything except main.cpp) so the test suite
# can link the real engine code without dragging in the app's main(). The
# app target links the library + main.cpp + the QML module.
find_package(Threads REQUIRED)
file(GLOB_RECURSE TFORGE_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
)
list(FILTER TFORGE_LIB_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/external/.*")
list(FILTER TFORGE_LIB_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_SOURCE_DIR}/src/main\\.cpp$")

add_library(temporal_forge_lib STATIC ${TFORGE_LIB_SOURCES} ${FSR4_SHADER_FILES})
target_include_directories(temporal_forge_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/vulkan_include"
    ${FFMPEG_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS})
target_link_libraries(temporal_forge_lib PUBLIC
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Widgets
    ${VULKAN_LIBRARIES}
    Threads::Threads ${CMAKE_DL_LIBS})
foreach(lib ${FFMPEG_LIBRARIES})
    target_link_libraries(temporal_forge_lib PUBLIC ${lib})
endforeach()
target_link_directories(temporal_forge_lib PUBLIC ${FFMPEG_LIBRARY_DIRS})
# Shader headers are generated into ${CMAKE_BINARY_DIR}/shaders/fsr4 by the
# custom shader-compile step. The lib owns the FSR4 harness which #includes
# them, so the shader sources must be part of the lib's build graph.
target_include_directories(temporal_forge_lib PUBLIC
    "${CMAKE_BINARY_DIR}/shaders/fsr4")

# --- main app target ---
qt_add_executable(temporal_forge_player src/main.cpp)
target_link_libraries(temporal_forge_player PRIVATE temporal_forge_lib)

target_include_directories(temporal_forge_player PRIVATE
    "${CMAKE_BINARY_DIR}/shaders/fsr4"
)
# miniaudio (Linux) dlopens the audio backend; keep its libs actually linked.
target_link_options(temporal_forge_player PRIVATE -Wl,--no-as-needed)

# --- QML module (spec: Qt6 + QML) ---
# The app exposes its own QML module "TemporalForge" so main.cpp can
# loadFromModule. UI C++ bridge types will be added here in later phases.
qt_add_qml_module(temporal_forge_player
    URI TemporalForge
    VERSION 1.0
    QML_FILES
        resources/qml/Main.qml
        resources/qml/IconButton.qml
        resources/qml/LiveHud.qml
        resources/qml/SettingsDialog.qml
        resources/qml/SettingsPane.qml
        resources/qml/SettingsSection.qml
        resources/qml/LabeledSlider.qml
        resources/qml/LabeledToggle.qml
        resources/qml/LabeledRadio.qml
        resources/qml/VideoContextMenu.qml
    RESOURCES
        resources/icons/app.svg
)

# --- install / runtime ---
set_target_properties(temporal_forge_player PROPERTIES
    WIN32_EXECUTABLE FALSE
    MACOSX_BUNDLE FALSE
)

# --- tests ---
enable_testing()
add_subdirectory(tests)

message(STATUS "Temporal Forge Player configured:")
message(STATUS "  FFmpeg: ${FFMPEG_VERSION} (libs: ${FFMPEG_LIBRARIES})")
message(STATUS "  Vulkan: ${VULKAN_VERSION}")
message(STATUS "  Qt6:    ${Qt6_VERSION}")
