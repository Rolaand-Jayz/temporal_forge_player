# tests/CMakeLists.txt — declare the focused contract tests for the player.
# Upstream: the real src library and small test fixtures. Downstream: ctest
# executables that validate math, configuration, media lifetime, backend
# contracts, and optional GPU diagnostics without changing runtime code.
#
# tests/CMakeLists.txt
# Headless unit tests. Run with: ctest --output-on-failure
#
# Tests link against the same util/ headers used by the app; for tests that
# only need headers (FsrTargetMath, Jitter, UpscaleTypes) we compile each
# test as a standalone executable with just the required sources.

function(tforge_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/external/vulkan_include")
    target_compile_features(${name} PRIVATE cxx_std_23)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

tforge_add_test(preset_math_tests
    SOURCES preset_math_tests.cpp
            ../src/backend/UpscaleTypes.cpp)

tforge_add_test(quality_lab_config_tests
    SOURCES quality_lab_config_tests.cpp
            ../src/config/QualityLabConfig.cpp)
target_link_libraries(quality_lab_config_tests PRIVATE Qt6::Core)

tforge_add_test(jitter_tests
    SOURCES jitter_tests.cpp)

tforge_add_test(frame_identity_tests
    SOURCES frame_identity_tests.cpp
            ../src/backend/UpscaleTypes.cpp)
target_compile_definitions(frame_identity_tests PRIVATE
    TFORGE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")

tforge_add_test(backend_tests
    SOURCES backend_tests.cpp
            ../src/backend/UpscaleTypes.cpp
            ../src/backend/NullBackend.cpp
            ../src/backend/SpatialFallbackBackend.cpp
            ../src/backend/Fsr23SdkBackend.cpp
            ../src/backend/Fsr4Int8Backend.cpp
            ../src/backend/Fsr3FallbackBackend.cpp
            ../src/backend/BackendSelector.cpp
            ../src/backend/Fsr4PostpassParams.cpp
            ../src/render/Fsr4DispatchHarness.cpp
            ../src/render/fsr4/Fsr4ConvSteps.cpp
            ../src/render/fsr4/Fsr4Memory.cpp
            ../src/util/Log.cpp)
target_include_directories(backend_tests PRIVATE "${CMAKE_BINARY_DIR}/shaders/fsr4")
target_link_libraries(backend_tests PRIVATE ${VULKAN_LIBRARIES})
add_dependencies(backend_tests temporal_forge_player)

tforge_add_test(sidebuffer_tests
    SOURCES sidebuffer_tests.cpp
            ../src/render/SideBufferSynth.cpp)

# FSR4 RE weight-blob loader test (no GPU needed; validates the RE dataset).
tforge_add_test(fsr4_weight_tests
    SOURCES fsr4_weight_tests.cpp
            ../src/backend/WeightBlob.cpp
            ../src/util/Log.cpp)
set_tests_properties(fsr4_weight_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M1 postpass contract test. This stays CPU-only: it validates the recovered
# byte region, host decode rules, and source-level shader consumption evidence.
tforge_add_test(fsr4_postpass_contract_tests
    SOURCES fsr4_postpass_contract_tests.cpp
            ../src/backend/Fsr4PostpassParams.cpp)
set_tests_properties(fsr4_postpass_contract_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M2 temporal resource-graph contract test. Runtime GPU validation is a later
# part of the milestone because the existing live harness test is disabled.
tforge_add_test(fsr4_temporal_contract_tests
    SOURCES fsr4_temporal_contract_tests.cpp)
set_tests_properties(fsr4_temporal_contract_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M3 causal codec-motion boundary test.
tforge_add_test(fsr4_motion_contract_tests
    SOURCES fsr4_motion_contract_tests.cpp)
set_tests_properties(fsr4_motion_contract_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M4 decode-to-upload color contract test. It runs before implementation work
# is accepted and keeps metadata, chroma phase, and high-bit-depth handling
# from regressing behind a passing shader compile.
tforge_add_test(color_metadata_contract_tests
    SOURCES color_metadata_contract_tests.cpp)
set_tests_properties(color_metadata_contract_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# M5 decoded-video jitter policy test. This runs the public side-buffer policy
# boundary without starting the disabled live GPU harness.
tforge_add_test(jitter_policy_tests
    SOURCES jitter_policy_tests.cpp
            ../src/render/SideBufferSynth.cpp
            ../src/util/Log.cpp)
set_tests_properties(jitter_policy_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# FSR4 tensor-map parser test (no GPU needed).
tforge_add_test(fsr4_tensormap_tests
    SOURCES fsr4_tensormap_tests.cpp
            ../src/backend/Fsr4TensorMap.cpp
            ../src/util/Log.cpp)
set_tests_properties(fsr4_tensormap_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    SKIP_RETURN_CODE 77)

# Standalone GPU capability probe (not a ctest — run manually; needs a GPU).
tforge_add_test(gpu_probe
    SOURCES gpu_probe.cpp
            ../src/backend/GpuCapabilityProbe.cpp
            ../src/util/Log.cpp)
target_link_libraries(gpu_probe PRIVATE ${VULKAN_LIBRARIES})
# Not run as part of ctest (it's an interactive diagnostic).
set_tests_properties(gpu_probe PROPERTIES DISABLED ON)

# Cooperative-matrix mode dumper (diagnostic).
tforge_add_test(cm_dump
    SOURCES cm_dump.cpp)
target_link_libraries(cm_dump PRIVATE ${VULKAN_LIBRARIES})
set_tests_properties(cm_dump PROPERTIES DISABLED ON)

# FSR4 dispatch harness + proof runner integration test (P-B/P-E). Live AMD GPU.
tforge_add_test(fsr4_harness_tests
    SOURCES fsr4_harness_tests.cpp
            ../src/backend/GpuCapabilityProbe.cpp
            ../src/backend/Fsr4PostpassParams.cpp
            ../src/backend/WeightBlob.cpp
            ../src/backend/Fsr4ProofRunner.cpp
            ../src/render/Fsr4DispatchHarness.cpp
            ../src/render/fsr4/Fsr4ConvSteps.cpp
            ../src/render/fsr4/Fsr4Memory.cpp
            ../src/util/Log.cpp)
target_link_libraries(fsr4_harness_tests PRIVATE ${VULKAN_LIBRARIES})
target_include_directories(fsr4_harness_tests PRIVATE "${CMAKE_BINARY_DIR}/shaders/fsr4")
# Ensure the embedded SPIR-V header is generated before compiling.
add_dependencies(fsr4_harness_tests temporal_forge_player)
set_tests_properties(fsr4_harness_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DISABLED ON)

# --- integration test (needs FFmpeg) ---
find_program(FFMPEG_BIN ffmpeg)
# Generate a deterministic sample clip as a test fixture.
if(FFMPEG_BIN)
    set(SAMPLE_MP4 "${CMAKE_CURRENT_BINARY_DIR}/sample.mp4")
    add_custom_command(
        OUTPUT ${SAMPLE_MP4}
        COMMAND ${FFMPEG_BIN} -y -loglevel error
            -f lavfi -i "testsrc2=size=640x360:rate=30:duration=5"
            -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=5"
            -c:v libx264 -pix_fmt yuv420p -color_range tv -colorspace bt709
            -c:a aac ${SAMPLE_MP4}
        DEPENDS ""
        COMMENT "Generating sample.mp4 test fixture"
        VERBATIM)
    add_custom_target(tforge_sample_mp4 DEPENDS ${SAMPLE_MP4})
endif()

# media_pipeline_tests exercises the real demux+decode path.
tforge_add_test(media_pipeline_tests
    SOURCES media_pipeline_tests.cpp
            ../src/media/Demuxer.cpp
            ../src/media/VideoDecoder.cpp
            ../src/media/AudioDecoder.cpp
            ../src/util/Log.cpp)
pkg_check_modules(FFMPEG_TEST IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
target_link_libraries(media_pipeline_tests PRIVATE PkgConfig::FFMPEG_TEST)
if(TARGET tforge_sample_mp4)
    add_dependencies(media_pipeline_tests tforge_sample_mp4)
    # Run from the build/tests dir where sample.mp4 is generated.
    set_tests_properties(media_pipeline_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "TFORGE_SAMPLE=${CMAKE_CURRENT_BINARY_DIR}/sample.mp4")
endif()

# file_switch_tests exercises the PlaybackEngine's openUrl/close cycle to
# verify that switching files mid-playback no longer deadlocks. It links
# against temporal_forge_lib so it reuses the real engine code without
# pulling in main.cpp or the QML module.
if(TARGET temporal_forge_lib AND TARGET tforge_sample_mp4)
    add_executable(file_switch_tests file_switch_tests.cpp)
    target_link_libraries(file_switch_tests PRIVATE temporal_forge_lib)
    target_compile_features(file_switch_tests PRIVATE cxx_std_23)
    add_dependencies(file_switch_tests temporal_forge_player tforge_sample_mp4)
    add_test(NAME file_switch_tests COMMAND file_switch_tests
        "${CMAKE_CURRENT_BINARY_DIR}/sample.mp4"
        "${CMAKE_CURRENT_BINARY_DIR}/sample.mp4")
endif()
