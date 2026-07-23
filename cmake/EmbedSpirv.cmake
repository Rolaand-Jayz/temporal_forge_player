# cmake/EmbedSpirv.cmake — embed a SPIR-V blob as a C header.
# Emits a uint32_t array + size constants. SPIR-V is little-endian uint32.
file(READ "${SPIRV_FILE}" spv_bytes HEX)
string(LENGTH "${spv_bytes}" nbytes_hex)
math(EXPR nbytes "(${nbytes_hex} / 2)")
math(EXPR nwords "(${nbytes} + 3) / 4")

set(words "")
set(word_index 0)
set(line "")
set(i 0)
while(i LESS nbytes)
    # Read up to 4 bytes (8 hex chars); pad with 00 if at the tail.
    set(j 0)
    set(hexword "")
    while(j LESS 4)
        math(EXPR bytepos "${i} + ${j}")
        if(bytepos LESS nbytes)
            math(EXPR hpos "${bytepos} * 2")
            string(SUBSTRING "${spv_bytes}" ${hpos} 2 byte)
        else()
            set(byte "00")
        endif()
        set(hexword "${byte}${hexword}")  # prepend for little-endian -> big-endian display
        math(EXPR j "${j} + 1")
    endwhile()
    set(word "0x${hexword}")
    set(line "${line}${word},")
    math(EXPR word_index "${word_index} + 1")
    if(word_index GREATER_EQUAL 8)
        set(words "${words}    ${line}\n")
        set(line "")
        set(word_index 0)
    endif()
    math(EXPR i "${i} + 4")
endwhile()
if(line)
    set(words "${words}    ${line}\n")
endif()

file(WRITE "${HEADER_FILE}"
"// Auto-generated from ${SPIRV_FILE}. Do not edit.
#pragma once
#include <cstdint>
namespace temporal_forge {
constexpr uint32_t k${SYMBOL}_spv_words = ${nwords};
constexpr uint32_t k${SYMBOL}_spv_bytes = ${nbytes};
extern const uint32_t k${SYMBOL}_spv[${nwords}];
} // namespace temporal_forge
const uint32_t temporal_forge::k${SYMBOL}_spv[${nwords}] = {
${words}};
")
