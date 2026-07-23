# cmake/ShaderCompile.cmake — compile FSR4 GLSL compute shaders to SPIR-V and
# embed them as C headers so the binary is self-contained.
#
# Produces for each shader:
#   <outdir>/<name>.spv           (compiled SPIR-V)
#   <outdir>/<name>.spv.h         (uint32_t array embedded as k<Name>_spv[])
#
# Usage in a CMakeLists:
#   include(${CMAKE_SOURCE_DIR}/cmake/ShaderCompile.cmake)
#   tforge_compile_shaders(
#       OUT_VAR FSR4_SHADER_HEADERS
#       OUT_DIR ${CMAKE_BINARY_DIR}/shaders/fsr4
#       SOURCES prepass_pq_eotf.comp conv_dw_dot4.comp ...)
#   target_sources(app PRIVATE ${FSR4_SHADER_HEADERS})
#   target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/shaders/fsr4)

find_program(GLSLANG glslangValidator REQUIRED
    HINTS /usr/sbin /usr/bin ENV PATH)

function(tforge_compile_shaders)
    set(options)
    set(oneValueArgs OUT_VAR OUT_DIR)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(generated)
    file(MAKE_DIRECTORY ${ARG_OUT_DIR})

    foreach(src_abs ${ARG_SOURCES})
        get_filename_component(name ${src_abs} NAME_WE)
        set(spv ${ARG_OUT_DIR}/${name}.spv)
        set(hdr ${ARG_OUT_DIR}/${name}.spv.h)
        set(sym ${name})
        # Make a CamelCase symbol for the C array: prepass_pq_eotf -> prepass_pq_eotf_spv
        add_custom_command(
            OUTPUT ${spv} ${hdr}
            COMMAND ${GLSLANG} -V ${src_abs} -o ${spv}
            COMMAND ${CMAKE_COMMAND}
                -DSPIRV_FILE=${spv}
                -DHEADER_FILE=${hdr}
                -DSYMBOL=${sym}
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            DEPENDS ${src_abs}
            COMMENT "GLSL->SPIR-V ${name}.comp"
            VERBATIM)
        list(APPEND generated ${spv} ${hdr})
    endforeach()

    set(${ARG_OUT_VAR} ${generated} PARENT_SCOPE)
endfunction()
