if(NOT DEFINED OUTPUT OR "${OUTPUT}" STREQUAL "")
    message(FATAL_ERROR "OUTPUT path is required")
endif()

string(REGEX REPLACE "^\"(.*)\"$" "\\1" OUTPUT "${OUTPUT}")
if(DEFINED INPUT)
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" INPUT "${INPUT}")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(HEADER_CONTENT "#pragma once\n#include <cstddef>\n\n")

if(DEFINED INPUT AND NOT "${INPUT}" STREQUAL "" AND EXISTS "${INPUT}")
    file(READ "${INPUT}" FONT_HEX HEX)
    string(REGEX MATCHALL ".." FONT_BYTES "${FONT_HEX}")

    string(APPEND HEADER_CONTENT "static constexpr unsigned char kBcduFontData[] = {\n")
    set(BYTE_COUNT 0)
    foreach(BYTE ${FONT_BYTES})
        if(BYTE_COUNT EQUAL 0)
            string(APPEND HEADER_CONTENT "    ")
        endif()

        string(APPEND HEADER_CONTENT "0x${BYTE}, ")
        math(EXPR BYTE_COUNT "${BYTE_COUNT} + 1")

        if(BYTE_COUNT EQUAL 12)
            string(APPEND HEADER_CONTENT "\n")
            set(BYTE_COUNT 0)
        endif()
    endforeach()

    if(NOT BYTE_COUNT EQUAL 0)
        string(APPEND HEADER_CONTENT "\n")
    endif()

    string(APPEND HEADER_CONTENT "};\n")
    string(APPEND HEADER_CONTENT "static constexpr std::size_t kBcduFontSize = sizeof(kBcduFontData);\n")
else()
    string(APPEND HEADER_CONTENT "static constexpr unsigned char kBcduFontData[] = {0};\n")
    string(APPEND HEADER_CONTENT "static constexpr std::size_t kBcduFontSize = 0;\n")
endif()

file(WRITE "${OUTPUT}" "${HEADER_CONTENT}")
