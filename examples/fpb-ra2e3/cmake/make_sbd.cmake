# make_sbd.cmake - create the CMSIS-Zone "secure bundle" (.sbd) that the
# Renesas hardware debugger (renesas-hardware) expects next to the built .elf.
#
# Why this exists:
#   The Renesas VS Code debugger opens "<elf-basename>.sbd" next to the .elf to
#   read the device memory partition (CMSIS-Zone: FLASH/DATA_FLASH/RAM regions).
#   e2 studio produces this bundle during its build; the plain CMake build does
#   not, so launching the debugger fails with "Invalid filename" (the adm-zip
#   library cannot find the .sbd). We recreate the bundle from the RASC-generated
#   .secure_* files so F5 / the CMake Debug button work.
#
# The .sbd is simply a ZIP whose entry names end in "rzone"/"azone" (+ an
# optional secure.xml). For non-TrustZone parts only the .rzone is required.
#
# Usage (invoked from a POST_BUILD step):
#   cmake -DSRC_DIR=<abs project dir> -DOUT_SBD=<abs .sbd path> -P make_sbd.cmake

if(NOT DEFINED SRC_DIR OR NOT DEFINED OUT_SBD)
    message(FATAL_ERROR "make_sbd.cmake requires -DSRC_DIR and -DOUT_SBD")
endif()

set(_rzone "${SRC_DIR}/.secure_rzone")
if(NOT EXISTS "${_rzone}")
    # No CMSIS-Zone info generated for this project (e.g. non-secure device
    # variant). Nothing to bundle - leave silently so the build still succeeds.
    return()
endif()

get_filename_component(_outdir "${OUT_SBD}" DIRECTORY)
set(_stage "${_outdir}/.sbd_stage")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}")

# Stage the RASC-generated zone files under names ending in rzone/azone/xml.
configure_file("${_rzone}" "${_stage}/secure.rzone" COPYONLY)
set(_files secure.rzone)
if(EXISTS "${SRC_DIR}/.secure_azone")
    configure_file("${SRC_DIR}/.secure_azone" "${_stage}/secure.azone" COPYONLY)
    list(APPEND _files secure.azone)
endif()
if(EXISTS "${SRC_DIR}/.secure_xml")
    configure_file("${SRC_DIR}/.secure_xml" "${_stage}/secure.xml" COPYONLY)
    list(APPEND _files secure.xml)
endif()

# Zip the staged files into the .sbd (standard zip the debugger's adm-zip reads).
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "${OUT_SBD}" --format=zip ${_files}
    WORKING_DIRECTORY "${_stage}"
    RESULT_VARIABLE _rc)
file(REMOVE_RECURSE "${_stage}")

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "make_sbd.cmake: failed to create ${OUT_SBD}")
endif()
message(STATUS "Created CMSIS-Zone debug bundle: ${OUT_SBD}")
