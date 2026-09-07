cmake_minimum_required(VERSION 3.15)
project(schwung-vavra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Build position-independent code (required for shared library)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# No STB_GNU_UNIQUE symbols.
#
# glibc marks any DSO that DEFINES a unique symbol as NODELETE at relocation
# time, so dlclose() on it is a no-op: the chain host unloads the slot, dlopen()
# hands back the same mapping, and the process keeps running the OLD dsp.so
# until MoveOriginal is restarted. Gearmulator's templates gave us 254 of them,
# which cost a whole debugging session -- ui_hierarchy kept answering with a
# build two commits behind the file on disk, at an identical load base.
#
# Set before the add_subdirectory() calls: the unique symbols come from mqLib /
# dsp56300 / synthLib linked into dsp.so, not from our own translation unit.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique>)
endif()

find_package(Threads REQUIRED)

# ---- Gearmulator dependencies ----
if(NOT GEARMULATOR_SOURCE)
    set(GEARMULATOR_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/libs/gearmulator/source)
endif()

# DSP56300 emulator. The microQ's DSP is JIT-compiled on Move (ARM64 allows it),
# which is where all of its speed comes from -- see docs/PERFORMANCE.md.
set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
add_subdirectory(${GEARMULATOR_SOURCE}/cpu/dsp56300/source ${CMAKE_BINARY_DIR}/dsp56300)
add_subdirectory(${GEARMULATOR_SOURCE}/framework/baseLib ${CMAKE_BINARY_DIR}/baseLib)
add_subdirectory(${GEARMULATOR_SOURCE}/framework/synthLib ${CMAKE_BINARY_DIR}/synthLib)
add_subdirectory(${GEARMULATOR_SOURCE}/3rdparty/libresample ${CMAKE_BINARY_DIR}/libresample)
add_subdirectory(${GEARMULATOR_SOURCE}/framework/hardwareLib ${CMAKE_BINARY_DIR}/hardwareLib)

# The microQ is a 68k microcontroller driving one DSP56300, so unlike the
# JP-8000 this needs mc68k as well as the DSP.
add_subdirectory(${GEARMULATOR_SOURCE}/cpu/mc68k ${CMAKE_BINARY_DIR}/mc68k)
target_include_directories(68kEmu PUBLIC ${GEARMULATOR_SOURCE}/cpu)

add_subdirectory(${GEARMULATOR_SOURCE}/waldi/common/wLib ${CMAKE_BINARY_DIR}/wLib)
add_subdirectory(${GEARMULATOR_SOURCE}/waldi/microq/mqLib ${CMAKE_BINARY_DIR}/mqLib)

# ---- The Move plugin ----

add_library(dsp SHARED src/dsp/vavra_plugin.cpp)
set_target_properties(dsp PROPERTIES PREFIX "" OUTPUT_NAME "dsp")
target_include_directories(dsp PRIVATE ${GEARMULATOR_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dsp PRIVATE mqLib synthLib baseLib Threads::Threads)
# Move's libstdc++ predates the builder's condition_variable ABI. Keep the
# bundled runtime private, including its GNU_UNIQUE locale symbols.
target_link_options(dsp PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)

add_executable(module_smoke tests/module_smoke.cpp)
target_include_directories(module_smoke PRIVATE src)
target_link_libraries(module_smoke PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
add_executable(runtime_test tests/runtime_test.cpp)
target_include_directories(runtime_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime_test PRIVATE Threads::Threads)
target_compile_options(runtime_test PRIVATE -UNDEBUG)

add_executable(dump_presets tools/dump_presets.cpp)
target_include_directories(dump_presets PRIVATE ${GEARMULATOR_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dump_presets PRIVATE mqLib synthLib baseLib Threads::Threads)
target_link_options(dump_presets PRIVATE -static-libstdc++ -static-libgcc)
