# RE4 PSP port: the second build target. Compiles the matching tree's game sources (never the
# Nintendo SDK / CRI / SN runtime under src/lib) with the PSP toolchain and RE4_PORT defined, plus
# the platform layer under port/src. The matching build (configure.py + ninja) is untouched.
#
#   export PSPDEV=$HOME/pspdev/pspdev   # pspdev release, see port/README.md
#   cmake -S port -B build/port -G Ninja
#   ninja -C build/port
#
# Milestones: (1) every game unit compiles (this file's OBJECT libraries), (2) the DOL units link
# against the platform layer into an EBOOT, (3) the REL modules as PRX or static.
cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{PSPDEV})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{PSPDEV}/psp/share/pspdev.cmake")
    else()
        message(FATAL_ERROR "set PSPDEV to the pspdev toolchain root (contains bin/psp-g++)")
    endif()
endif()

project(re4port LANGUAGES C CXX)

set(RE4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_filename_component(RE4_ROOT "${RE4_ROOT}" ABSOLUTE)

# Unit list from the matching build's configuration (tools/port/units.py).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(RE4_UNITS_CMAKE "${CMAKE_BINARY_DIR}/units.cmake")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${RE4_ROOT}/tools/port/units.py" "${RE4_UNITS_CMAKE}"
    RESULT_VARIABLE units_result
    OUTPUT_VARIABLE units_output)
if(NOT units_result EQUAL 0)
    message(FATAL_ERROR "tools/port/units.py failed: ${units_output}")
endif()
message(STATUS "${units_output}")
include("${RE4_UNITS_CMAKE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RE4_ROOT}/config/G4BE08/objects.py"
    "${RE4_ROOT}/config/G4BE08/modules.py"
    "${RE4_ROOT}/tools/port/units.py")

# Flags shared by every game unit. -funsigned-char: the ProDG build ran with __CHAR_UNSIGNED__
# (PowerPC default); -G0: no gp-relative small data (the game's globals are large); -fpermissive /
# gnu++14: the code is GCC 2.95 C++.
add_library(re4_flags INTERFACE)
target_include_directories(re4_flags INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${RE4_ROOT}/include"
    "${RE4_ROOT}/src")
target_compile_definitions(re4_flags INTERFACE RE4_PORT=1 VERSION_G4BE08=1 BUILD_VERSION=0 NDEBUG=1)
target_compile_options(re4_flags INTERFACE
    -O2 -G0 -funsigned-char -fno-strict-aliasing -fpermissive -w
    -mno-check-zero-division  # the GameCube does not trap on integer division by zero either (the PSP build would `break`)
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++14 -fno-exceptions -fno-rtti -fno-sized-deallocation>)

function(re4_sources out)
    set(list "")
    foreach(u IN LISTS ARGN)
        list(APPEND list "${RE4_ROOT}/src/${u}")
    endforeach()
    set(${out} "${list}" PARENT_SCOPE)
endfunction()

# The DOL units.
re4_sources(dol_sources ${RE4_DOL_UNITS})
add_library(re4_game OBJECT ${dol_sources})
target_link_libraries(re4_game PRIVATE re4_flags)

# The REL modules, one object library each (a source shared by several modules is compiled once
# per module, with REL_MODULE naming it, as the matching build does). Per-module defines mirror
# config/G4BE08/modules.py CFLAGS.
set(RE4_MODULE_DEFS_t_id TOOLS_ARRAY)
set(RE4_MODULE_DEFS_t_esp TOOLS_ARRAY TOOLS_EM_ARRAY)
set(RE4_MODULE_DEFS_Tools TOOLS_ARRAY)
set(re4_module_targets "")
foreach(mod IN LISTS RE4_MODULES)
    if(mod STREQUAL "Tools" OR mod MATCHES "^t_")
        continue()  # the developers' debug editors: not part of the port (host file server, SN tools)
    endif()
    re4_sources(mod_sources ${RE4_MODULE_${mod}})
    if(mod_sources)
        add_library(re4_mod_${mod} OBJECT ${mod_sources})
        target_link_libraries(re4_mod_${mod} PRIVATE re4_flags)
        target_compile_definitions(re4_mod_${mod} PRIVATE REL_MODULE=${mod} ${RE4_MODULE_DEFS_${mod}}
            _prolog=re4_${mod}_prolog _epilog=re4_${mod}_epilog _unresolved=re4_${mod}_unresolved)
        list(APPEND re4_module_targets re4_mod_${mod})
    endif()
endforeach()

# The platform layer (port/src): what src/lib provided on the GameCube, on the PSP.
file(GLOB port_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
# The SDK's C sources the port reuses unchanged (hardware-free), and its C matrix / vector /
# quaternion functions extracted from the paired-single units (tools/port/extract_sdk_c.py).
set(RE4_SDK_MTX "${CMAKE_BINARY_DIR}/sdk_mtx.c")
add_custom_command(
    OUTPUT "${RE4_SDK_MTX}"
    COMMAND "${Python3_EXECUTABLE}" "${RE4_ROOT}/tools/port/extract_sdk_c.py" "${RE4_SDK_MTX}"
    DEPENDS "${RE4_ROOT}/tools/port/extract_sdk_c.py" "${RE4_ROOT}/src/lib/mtx.c" "${RE4_ROOT}/src/lib/mtx44.c"
            "${RE4_ROOT}/src/lib/mtxvec.c" "${RE4_ROOT}/src/lib/mtx44vec.c" "${RE4_ROOT}/src/lib/quat.c" "${RE4_ROOT}/src/lib/vec.c"
    COMMENT "Extracting the SDK's C matrix functions")
add_library(re4_port STATIC ${port_sources} "${RE4_ROOT}/src/lib/OSAlloc.c" "${RE4_SDK_MTX}")
target_link_libraries(re4_port PRIVATE re4_flags)

# `ninja compile_all`: milestone 1, every unit compiles.
add_custom_target(compile_all DEPENDS re4_game ${re4_module_targets} re4_port)

# The REL modules, linked statically (tools/port/modlink.py): one .mod.o per module with only its
# entry points and section markers global, plus the registry port/src/port_modules.cpp reads.
set(RE4_MODS_DIR "${CMAKE_BINARY_DIR}/mods")
set(re4_mod_objects "")
foreach(mod IN LISTS RE4_MODULES)
    if(TARGET re4_mod_${mod} AND NOT mod STREQUAL "Tools" AND NOT mod MATCHES "^t_")
        list(APPEND re4_mod_objects "${RE4_MODS_DIR}/${mod}.mod.o")  # the debug tool modules stay out
    endif()
endforeach()
set(RE4_MODULES_GEN "${RE4_MODS_DIR}/port_modules_gen.cpp")
add_custom_command(
    OUTPUT ${re4_mod_objects} "${RE4_MODULES_GEN}"
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=$ENV{PSPDEV}/bin:$ENV{PATH}" "${Python3_EXECUTABLE}" "${RE4_ROOT}/tools/port/modlink.py"
            --build "${CMAKE_BINARY_DIR}" --out "${RE4_MODS_DIR}"
    DEPENDS ${re4_module_targets} re4_game re4_port "${RE4_ROOT}/tools/port/modlink.py"
    COMMENT "Linking the REL modules")
set_source_files_properties(${re4_mod_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# Milestone 2: the DOL units and the modules linked against the platform layer, missing SDK entry
# points as generated weak stubs (tools/port/gen_stubs.py, regenerated whenever the objects change).
set(RE4_STUBS "${CMAKE_BINARY_DIR}/sdk_stubs.cpp")
set(RE4_STUBS_DOLPHIN "${CMAKE_BINARY_DIR}/sdk_stubs_dolphin.cpp")
add_custom_command(
    OUTPUT "${RE4_STUBS}" "${RE4_STUBS_DOLPHIN}"
    COMMAND "${CMAKE_COMMAND}" -E env "PSPDEV=$ENV{PSPDEV}" "${Python3_EXECUTABLE}" "${RE4_ROOT}/tools/port/gen_stubs.py"
            --objects "${CMAKE_BINARY_DIR}/CMakeFiles/re4_game.dir"
            --objects "${RE4_MODS_DIR}"
            --defined "${CMAKE_BINARY_DIR}/CMakeFiles/re4_port.dir"
            --out "${RE4_STUBS}"
    DEPENDS re4_game re4_port ${re4_mod_objects} "${RE4_ROOT}/tools/port/gen_stubs.py"
    COMMENT "Generating SDK stubs")
add_executable(re4 "${CMAKE_CURRENT_SOURCE_DIR}/app/psp_main.cpp" "${RE4_STUBS}" "${RE4_STUBS_DOLPHIN}" "${RE4_MODULES_GEN}"
    $<TARGET_OBJECTS:re4_game> ${re4_mod_objects})
# User-mode PSP libraries only: the driver's own spec appends libc, libm, libstdc++, libpspsdk and
# libpspuser after these. libpspkernel must not appear: its StdioForKernel / SysclibForKernel
# import stubs would shadow libc's printf / strtol (unimplemented in user mode).
target_link_libraries(re4 PRIVATE re4_flags re4_port pspgu pspge pspdisplay pspdebug pspctrl pspaudio)
create_pbp_file(TARGET re4 TITLE "RE4 PSP port" MEMSIZE 1)
