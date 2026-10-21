# Please see README.md for build instructions

cmake_minimum_required(VERSION 3.16.4)

set(CMAKE_SYSTEM_NAME Generic)

set(CMAKE_SYSTEM_PROCESSOR arm)

# Use add_library() with the STATIC option to name the source file in the generated project.
# This avoids running the linker and is intended for use with cross-compiling toolchains that
# cannot link without custom flags or linker scripts
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo MinSizeRel)

if(BUILD_KIT)
	message(NOTICE "Build Kit is defined as ${BUILD_KIT}")
endif()

message(NOTICE "CMAKE_CURRENT_BINARY_DIR is defined as ${CMAKE_CURRENT_BINARY_DIR}")

get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(isMultiConfig)
    message(NOTICE "Multi-config generator(${CMAKE_GENERATOR}) is active")
else()
    message(NOTICE "Single-config generator(${CMAKE_GENERATOR}) is active")
	if(DEFINED CMAKE_BUILD_TYPE)
	 	message(NOTICE "CMAKE_BUILD_TYPE is defined as ${CMAKE_BUILD_TYPE}")
	elseif(DEFINED BUILD_TYPE)
		set(CMAKE_BUILD_TYPE ${BUILD_TYPE})
		message(NOTICE "CMAKE_BUILD_TYPE is set to BUILD_TYPE ENV ${CMAKE_BUILD_TYPE}")
	else()
		message(FATAL_ERROR "Please specify the CMAKE_BUILD_TYPE with -DCMAKE_BUILD_TYPE=")
	endif()
    #set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE})
	message(NOTICE "CMAKE_RUNTIME_OUTPUT_DIRECTORY is defined as ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

cmake_minimum_required(VERSION 3.16.4)

# Project configuration
project(ra2e3_ptx_Iot_fsp_writer
	VERSION 1.0.0
	LANGUAGES C CXX ASM)

set(CMAKE_CROSSCOMPILING "TRUE")
set(CMAKE_EXPORT_COMPILE_COMMANDS "TRUE")

include(${CMAKE_CURRENT_LIST_DIR}/Config.cmake)

# --------------------------------------------------------------------------
# Auto-generate missing FSP content
# --------------------------------------------------------------------------
# ra/, ra_cfg/, ra_gen/ and the generated linker scripts are not committed; they
# are produced by RASC from configuration.xml. cmake/GeneratedSrc.cmake globs
# them at configure time, so they must exist *before* it is included -- the
# RASC pre-build step in that file runs too late (and is disabled entirely
# when RENESAS_IDE=e2studio, as the VS Code kits set). If anything is missing,
# run cmake/prebuild.cmake now. Disable with -DRASC_AUTO_GENERATE=OFF.
option(RASC_AUTO_GENERATE "Run RASC at configure time if generated FSP content is missing" ON)

set(_rasc_generated_items ra ra_cfg ra_gen memory_regions.lld fsp_gen.lld)
set(_rasc_missing_items "")
foreach(_item IN LISTS _rasc_generated_items)
    set(_path ${CMAKE_CURRENT_SOURCE_DIR}/${_item})
    if(IS_DIRECTORY ${_path})
        file(GLOB _dir_content ${_path}/*)
        if(NOT _dir_content)
            list(APPEND _rasc_missing_items ${_item})
        endif()
    elseif(NOT EXISTS ${_path})
        list(APPEND _rasc_missing_items ${_item})
    endif()
endforeach()

if(_rasc_missing_items)
    if(NOT RASC_AUTO_GENERATE)
        message(FATAL_ERROR "Generated FSP content missing (${_rasc_missing_items}) and RASC_AUTO_GENERATE is OFF. "
                            "Run 'cmake -P cmake/prebuild.cmake' or generate from the Smart Configurator.")
    endif()
    if(NOT EXISTS "${RASC_GENERATE_EXE_PATH}")
        message(FATAL_ERROR "Generated FSP content missing (${_rasc_missing_items}) and RASC was not found at "
                            "'${RASC_GENERATE_EXE_PATH}'. Set the RASC_EXE_PATH env var or -DRASC_EXE_PATH=.")
    endif()
    message(NOTICE "Generated FSP content missing (${_rasc_missing_items}); running RASC to generate it...")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DRASC_EXE_PATH=${RASC_GENERATE_EXE_PATH}
            -DRASC_TOOLCHAIN_VERSION=${CMAKE_C_COMPILER_VERSION}
            -DRASC_BUILD_CONFIG=${CMAKE_BUILD_TYPE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prebuild.cmake
        RESULT_VARIABLE _rasc_result)
    if(NOT _rasc_result EQUAL 0)
        message(FATAL_ERROR "RASC generation failed (exit code ${_rasc_result})")
    endif()
    # The RASC-generated cmake/*.cmake files are rewritten by the generate, so
    # they are included only after it has finished.
endif()

# Re-run configure on the next build if any generated item is deleted, so
# pressing Build regenerates it instead of failing on missing files.
foreach(_item ra_gen/hal_data.c ra_cfg/fsp_cfg/bsp/bsp_cfg.h ra/fsp/inc/fsp_version.h memory_regions.lld fsp_gen.lld)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_item})
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratedCfg.cmake)

# If required user can amend RASC generated flags here, e.g. list(REMOVE_ITEM RASC_CMAKE_C_FLAGS "-Wall")

# --------------------------------------------------------------------------
# Code size: match the e2 studio (RA2E3_nfc_writer) build settings
# --------------------------------------------------------------------------
# The writer image only fits the R7FA2E3073CFL's 64 KB code flash with the
# same size settings as the e2 studio managed build (.cproject):
#   - -Oz (Optimize for size, aggressive) in every configuration; RASC
#     generates -O2/-Os, which overflow the flash
#   - link-time optimisation (-flto)
#   - identical code folding "safe" (-Wl,--icf=safe instead of RASC's none)
foreach(_rasc_opt_var RASC_DEBUG_FLAGS RASC_RELEASE_FLAGS RASC_MIN_SIZE_RELEASE_FLAGS RASC_RELEASE_WITH_DEBUG_INFO)
    list(TRANSFORM ${_rasc_opt_var} REPLACE "^-O[0-3sgz]?$" "-Oz")
endforeach()
list(APPEND RASC_CMAKE_C_FLAGS -flto)
list(APPEND RASC_CMAKE_CXX_FLAGS -flto)
list(TRANSFORM RASC_CMAKE_EXE_LINKER_FLAGS REPLACE "^-Wl,--icf=none$" "-Wl,--icf=safe")
list(APPEND RASC_CMAKE_EXE_LINKER_FLAGS -flto)

# e2 studio compiles the writer with -funsigned-char (Other optimization flags);
# RASC generates -fsigned-char. Use the e2 studio setting so both IDEs build
# identical code.
foreach(_rasc_flag_var RASC_CMAKE_ASM_FLAGS RASC_CMAKE_C_FLAGS RASC_CMAKE_CXX_FLAGS RASC_CMAKE_EXE_LINKER_FLAGS)
    list(TRANSFORM ${_rasc_flag_var} REPLACE "^-fsigned-char$" "-funsigned-char")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratedSrc.cmake)

# --------------------------------------------------------------------------
# Keep #ToolchainVersion# in configuration.xml
# --------------------------------------------------------------------------
# Every headless RASC invocation rewrites configuration.xml with an empty
# <option key="#ToolchainVersion#" value=""/>, which shows up as a spurious diff
# and can make e2 studio re-prompt for a toolchain version. GeneratedSrc.cmake is
# RASC-generated ("should not be edited by the user"), so the fix is bolted on
# here instead: after each target that shells out to RASC, re-run prebuild.cmake
# in RESTORE_ONLY mode, which repairs the field without regenerating anything.
foreach(_rasc_tgt generate_content_${PROJECT_NAME} generate_sbd_${PROJECT_NAME} ${PROJECT_NAME}.elf)
    if(TARGET ${_rasc_tgt})
        add_custom_command(TARGET ${_rasc_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DRESTORE_ONLY=ON
                -DRASC_TOOLCHAIN_VERSION=${CMAKE_C_COMPILER_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prebuild.cmake
            COMMENT "Restoring #ToolchainVersion# in configuration.xml"
            VERBATIM)
    endif()
endforeach()

# --------------------------------------------------------------------------
# User (application) additions
# --------------------------------------------------------------------------
# The RS NFC writer (with the NFC reader it builds on) is the product of this
# repository and lives at the top level:
#     <repo>/src       public headers (rs_nfc_writer.h, rs_nfc_reader.h) +
#                      implementation
#                      (+ src/ptx105r hardware layer)
#     <repo>/test      unit-test placeholders (not built into the firmware and
#                      not copied into this example)
# This example carries its own copy of <repo>/src under
# src/rs_nfc_card_writer/. The copy is ordinary, committed source -- not a
# filesystem link and not generated at build time -- so the project builds from
# a plain checkout with no pre-build step, and e2studio and VS Code compile
# byte-identical files. The trade-off is that the copy has to be refreshed by
# hand when the library at the repository root changes.
#
# Everything under src/ is therefore ordinary source: src/hal_entry.c,
# src/new_thread0_entry.c, src/app/, src/console_io/ and
# src/rs_nfc_card_writer/. It is all picked up by the GLOB_RECURSE over
# src/*.c in cmake/GeneratedSrc.cmake, which is the same mechanism the
# e2studio managed build uses. The unit tests live only at <repo>/test, so
# there is nothing under src/ to exclude from the firmware.

# Headers live in sub-folders, so add them to the include path so
# #include "rs_nfc_writer.h" etc. resolve. These mirror the include paths set
# on the e2studio configurations in .cproject.
target_include_directories(${PROJECT_NAME}.elf
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rs_nfc_card_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rs_nfc_card_writer/ptx105r
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${CMAKE_CURRENT_SOURCE_DIR}/src/console_io
)

# --------------------------------------------------------------------------
# Console: src/console_io (no libc stdio)
# --------------------------------------------------------------------------
# The application logs through APP_LOG -> console_io_printf(), a minimal
# printf (%c %s %d %u %x %X, width, zero pad) that writes directly to the FSP
# SCI UART, and reads CLI input with console_io_getc(). Nothing calls libc
# printf/putchar/getchar/snprintf, so picolibc stdio and its stdin/stdout FILE
# objects are not linked at all.
#
# If libc printf is ever needed again, it must be the integer-only variant
# (the default float-capable vfprintf does not fit in 64 KB flash):
#   target_link_options(${PROJECT_NAME}.elf PRIVATE -Wl,--defsym=vfprintf=__l_vfprintf)
# and the same option added in e2studio (Linker -> Other).

# --------------------------------------------------------------------------
# CMSIS-Zone secure bundle (.sbd) for the Renesas hardware debugger
# --------------------------------------------------------------------------
# The Renesas VS Code debugger (type "renesas-hardware") reads a
# "<elf-basename>.sbd" file next to the built .elf to obtain the device memory
# partition (CMSIS-Zone). e2 studio creates this during its build; the CMake
# build does not, so launching the debugger otherwise fails with
# "Invalid filename". Recreate it from the RASC-generated .secure_* files so
# F5 / the CMake Debug button work out of the box. (No-op if .secure_rzone is
# absent, e.g. a non-secure device variant.)
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUT_SBD=$<TARGET_FILE_DIR:${PROJECT_NAME}.elf>/${PROJECT_NAME}.sbd
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_sbd.cmake
    COMMENT "Generating ${PROJECT_NAME}.sbd (CMSIS-Zone bundle for Renesas debugger)"
    VERBATIM)
