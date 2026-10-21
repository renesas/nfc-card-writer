# Configuration file for custom user settings

# Locate the Smart Configurator (RASC) executable.
if(DEFINED RASC_EXE_PATH)
    set(_rasc_exe "${RASC_EXE_PATH}")
    message("Using RASC_EXE_PATH defined via CLI -D: ${_rasc_exe}")
elseif(DEFINED ENV{RASC_EXE_PATH})
    set(_rasc_exe "$ENV{RASC_EXE_PATH}")
    message("Using RASC_EXE_PATH defined in environment: ${_rasc_exe}")
else ()
    # No override given: auto-detect the newest installed Smart Configurator so
    # this works on any PC regardless of the exact sc_v*/fsp_v* version installed.
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _localapp)
    file(GLOB _rasc_candidates
        "C:/Renesas/RA/sc_v*/eclipse/rasc.exe"
        "${_localapp}/Programs/Renesas/RA/sc_v*/eclipse/rasc.exe")
    if(_rasc_candidates)
        # Pick the newest by the sc_v<date>_fsp_v<version> folder name, not by
        # the full path (C:/Renesas/... and %LOCALAPPDATA%/... would otherwise
        # sort by location instead of version).
        set(_rasc_keyed "")
        foreach(_c IN LISTS _rasc_candidates)
            string(REGEX MATCH "sc_v[^/]+" _ver "${_c}")
            list(APPEND _rasc_keyed "${_ver}|${_c}")
        endforeach()
        list(SORT _rasc_keyed COMPARE NATURAL)
        list(GET _rasc_keyed -1 _rasc_newest)
        string(REGEX REPLACE "^[^|]*\\|" "" _rasc_exe "${_rasc_newest}")
        message("Auto-detected RASC_EXE_PATH: ${_rasc_exe}")
    else()
        set(_rasc_exe "")
        message("RA Smart Configurator (RASC) not found - only needed to (re)generate "
                "ra/, ra_gen/, ra_cfg/. Set RASC_EXE_PATH to override.")
    endif()
endif()

# The real executable is always kept in RASC_GENERATE_EXE_PATH, so CMakeLists.txt
# can still generate missing FSP content (ra/, ra_cfg/, ra_gen/, ...) when
# RASC_EXE_PATH is replaced by "echo" below.
set(RASC_GENERATE_EXE_PATH "${_rasc_exe}")

if(RENESAS_IDE AND RENESAS_IDE STREQUAL "e2studio")
    set(RASC_EXE_PATH "echo")
    message(NOTICE "RENESAS_IDE is defined, so RASC_EXE_PATH set to echo for information purposes")
else()
    if(_rasc_exe STREQUAL "")
        message(FATAL_ERROR "Could not auto-detect a Renesas Smart Configurator (RASC) install. "
            "Set RASC_EXE_PATH via -DRASC_EXE_PATH=... or the RASC_EXE_PATH environment variable.")
    endif()
    set(RASC_EXE_PATH "${_rasc_exe}")
endif()
