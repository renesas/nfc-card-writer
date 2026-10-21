# Toolchain file for the LLVM / Arm Toolchain for Embedded (ATfE) toolchain.
#
# Same toolchain configuration as the reference projects (e.g.
# pes-ble-beacon-broadcast): the ATfE "bin" folder comes from
# -DARM_TOOLCHAIN_PATH=..., the ARM_TOOLCHAIN_PATH environment variable, or a
# VS Code CMake kit (see .vscode/cmake-kits.json). If none is set, the
# toolchain is auto-detected (Renesas Platform Manager / e2 studio installs).
# The compile/link/define flags come from cmake/GeneratedCfg.cmake.

# Toolchain binary directory.
# (Should avoid spaces in the path or have to escape them)
if(DEFINED ARM_TOOLCHAIN_PATH AND NOT ARM_TOOLCHAIN_PATH STREQUAL "")
  set(CMAKE_FIND_ROOT_PATH "${ARM_TOOLCHAIN_PATH}")
  message("Using ARM_TOOLCHAIN_PATH given with cmake settings -DARM_TOOLCHAIN_PATH=${ARM_TOOLCHAIN_PATH}")
elseif(DEFINED ENV{ARM_TOOLCHAIN_PATH} AND NOT ENV{ARM_TOOLCHAIN_PATH} STREQUAL "")
  message("Using ARM_TOOLCHAIN_PATH defined in environment: $ENV{ARM_TOOLCHAIN_PATH}")
  set(CMAKE_FIND_ROOT_PATH $ENV{ARM_TOOLCHAIN_PATH})
elseif(DEFINED ENV{ARM_LLVM_TOOLCHAIN_PATH} AND NOT ENV{ARM_LLVM_TOOLCHAIN_PATH} STREQUAL "")
  message("Using ARM_LLVM_TOOLCHAIN_PATH defined in environment: $ENV{ARM_LLVM_TOOLCHAIN_PATH}")
  set(CMAKE_FIND_ROOT_PATH $ENV{ARM_LLVM_TOOLCHAIN_PATH})
else ()
  # No path given: find the LLVM (ATfE) toolchain automatically.
  # 1. Tools registered by the Renesas Platform Manager (VS Code), read from
  #    %USERPROFILE%/.renesas/platform/state.json.
  # 2. Standard e2 studio / installer locations.
  # Version 21.1.1 (the one this project is validated with) is preferred;
  # otherwise the newest one found is used.
  file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _home)
  file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _localapp)
  set(_atfe_dirs "")
  set(_rp_state "${_home}/.renesas/platform/state.json")
  if(EXISTS "${_rp_state}")
    file(READ "${_rp_state}" _rp_json)
    string(JSON _n ERROR_VARIABLE _err LENGTH "${_rp_json}" installed arm-llvm versions)
    if(NOT _err AND _n GREATER 0)
      math(EXPR _last "${_n} - 1")
      foreach(_i RANGE ${_last})
        string(JSON _d ERROR_VARIABLE _err GET "${_rp_json}" installed arm-llvm versions ${_i} directory)
        if(NOT _err AND _d)
          file(TO_CMAKE_PATH "${_d}" _d)
          list(APPEND _atfe_dirs "${_d}" "${_d}/bin")
          file(GLOB _sub LIST_DIRECTORIES true "${_d}/*/bin")
          list(APPEND _atfe_dirs ${_sub})
        endif()
      endforeach()
    endif()
  endif()
  file(GLOB _atfe_glob LIST_DIRECTORIES true
    "${_home}/.renesas/platform/*/*/*/bin"
    "${_home}/.renesas/platform/*/*/bin"
    "${_localapp}/Programs/Renesas/*/toolchains/llvm_arm/ATfE-*/bin"
    "${_localapp}/Programs/Renesas/RA/*/toolchains/llvm_arm/ATfE-*/bin"
    "C:/Renesas/*/toolchains/llvm_arm/ATfE-*/bin"
    "C:/Renesas/RA/*/toolchains/llvm_arm/ATfE-*/bin")
  list(APPEND _atfe_dirs ${_atfe_glob})

  # Keep only folders that really contain the ATfE clang (not GNU toolchains).
  set(_atfe_found "")
  foreach(_dir IN LISTS _atfe_dirs)
    if((EXISTS "${_dir}/clang.exe" OR EXISTS "${_dir}/clang") AND
       (EXISTS "${_dir}/llvm-objcopy.exe" OR EXISTS "${_dir}/llvm-objcopy"))
      list(APPEND _atfe_found "${_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _atfe_found)

  if(_atfe_found)
    list(SORT _atfe_found COMPARE NATURAL)
    list(GET _atfe_found -1 ARM_TOOLCHAIN_PATH)
    foreach(_dir IN LISTS _atfe_found)
      if(_dir MATCHES "21\\.1\\.1")
        set(ARM_TOOLCHAIN_PATH "${_dir}")
      endif()
    endforeach()
    set(CMAKE_FIND_ROOT_PATH "${ARM_TOOLCHAIN_PATH}")
    message("Auto-detected LLVM (ATfE) toolchain: ${ARM_TOOLCHAIN_PATH}")
  else()
    message(FATAL_ERROR "LLVM (ATfE) toolchain not found. Install it (e2 studio or the "
                        "Renesas Platform Manager), or set the ARM_TOOLCHAIN_PATH "
                        "environment variable to the ATfE 'bin' folder.")
  endif()
endif()

# Ninja: if it is not on PATH, use the one registered by the Renesas Platform
# Manager (state.json) so the "Ninja" kit works without any PATH setup.
if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_MAKE_PROGRAM)
  find_program(_ninja_on_path NAMES ninja ninja-build)
  if(NOT _ninja_on_path)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _home)
    set(_rp_state "${_home}/.renesas/platform/state.json")
    if(EXISTS "${_rp_state}")
      file(READ "${_rp_state}" _rp_json)
      string(JSON _n ERROR_VARIABLE _err LENGTH "${_rp_json}" installed ninja-build versions)
      if(NOT _err AND _n GREATER 0)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
          string(JSON _d ERROR_VARIABLE _err GET "${_rp_json}" installed ninja-build versions ${_i} directory)
          if(NOT _err AND _d)
            file(TO_CMAKE_PATH "${_d}" _d)
            file(GLOB _nj "${_d}/ninja.exe" "${_d}/bin/ninja.exe" "${_d}/*/ninja.exe" "${_d}/ninja")
            if(_nj)
              list(GET _nj 0 _nj)
              set(CMAKE_MAKE_PROGRAM "${_nj}" CACHE FILEPATH "Ninja (Renesas Platform)" FORCE)
              message("Using Ninja registered by the Renesas Platform Manager: ${_nj}")
              break()
            endif()
          endif()
        endforeach()
      endif()
    endif()
  endif()
endif()

if(NOT EXISTS "${CMAKE_FIND_ROOT_PATH}" OR NOT IS_DIRECTORY "${CMAKE_FIND_ROOT_PATH}")
  message(FATAL_ERROR "Toolchain path does not exists: ${CMAKE_FIND_ROOT_PATH}")
endif()

if(NOT DEFINED CMAKE_SYSTEM_NAME)
  set(CMAKE_SYSTEM_NAME Generic)
endif()

if(NOT DEFINED CMAKE_SYSTEM_PROCESSOR)
  set(CMAKE_SYSTEM_PROCESSOR arm)
endif()

# Use add_library() with the STATIC option so the compiler check does not try to
# link (bare-metal clang cannot link without the FSP linker script).
if(NOT DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

# Propagate ARM_TOOLCHAIN_PATH into try_compile sub-projects
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARM_TOOLCHAIN_PATH)

if (CMAKE_HOST_WIN32)
  set(BINARY_FILE_EXT ".exe")
else()
  set(BINARY_FILE_EXT "")
endif()

set(TARGET_TRIPLE arm-none-eabi)

# Make clang cross-compile for bare-metal Arm by default. The generated flag
# lists in GeneratedCfg.cmake also pass --target, but setting it here makes the
# CMake compiler identification / try-compile use the right target too.
set(CMAKE_C_COMPILER_TARGET ${TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TARGET_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${TARGET_TRIPLE})

# CMake variables for compiler, assembler, and binary utilities (LLVM).
set(CMAKE_C_COMPILER   "${CMAKE_FIND_ROOT_PATH}/clang${BINARY_FILE_EXT}"   CACHE STRING "C Compiler")
set(CMAKE_CXX_COMPILER "${CMAKE_FIND_ROOT_PATH}/clang++${BINARY_FILE_EXT}" CACHE STRING "C++ Compiler")
set(CMAKE_ASM_COMPILER "${CMAKE_FIND_ROOT_PATH}/clang${BINARY_FILE_EXT}"   CACHE STRING "ASM Compiler")

# LLVM binutils (explicit, so behavior does not depend on CMake's compiler-sibling
# auto-detection heuristics across platforms/versions).
set(CMAKE_AR      "${CMAKE_FIND_ROOT_PATH}/llvm-ar${BINARY_FILE_EXT}"      CACHE STRING "Archiver")
set(CMAKE_RANLIB  "${CMAKE_FIND_ROOT_PATH}/llvm-ranlib${BINARY_FILE_EXT}"  CACHE STRING "Ranlib")
set(CMAKE_OBJCOPY "${CMAKE_FIND_ROOT_PATH}/llvm-objcopy${BINARY_FILE_EXT}" CACHE STRING "Objcopy")
set(CMAKE_OBJDUMP "${CMAKE_FIND_ROOT_PATH}/llvm-objdump${BINARY_FILE_EXT}" CACHE STRING "Objdump")
set(CMAKE_SIZE    "${CMAKE_FIND_ROOT_PATH}/llvm-size${BINARY_FILE_EXT}"    CACHE STRING "Size")

# ld.lld is invoked via the clang driver (-fuse-ld=lld), not as CMAKE_LINKER,
# so that clang still drives multilib/runtime library selection at link time.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Search for programs in the build host directories, but headers/libraries only
# in the target toolchain directories.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
