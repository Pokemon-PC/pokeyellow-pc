# Native PC build of a statically recompiled Game Boy (Color) game.
#
#   python pc/tools/recomp/recomp.py --rom pokecrystal.gbc --sym pokecrystal.sym --src . \
#          --out build/rc_en --name crystal --prefix en --lang en
#   cmake -S pc -B build/pc -G Ninja -DCMAKE_C_COMPILER=clang "-DRC_DIRS=build/rc_en;build/rc_fr"
#   cmake --build build/pc
cmake_minimum_required(VERSION 3.16)
project(gbrecomp C)

# RC_DIRS: directories written by recomp.py (one per translated ROM, each with
# its own --prefix); the executable runs whichever matches the player's ROM.
set(RC_DIRS "${CMAKE_SOURCE_DIR}/../build/rc_en" CACHE STRING "recompiled C directories (;-separated)")
set(GAME_NAME "pokeyellow" CACHE STRING "executable name")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SDL2: an already built static SDL2 (SDL2_PREBUILT_DIR = its build dir, with
# include/ and include-config-*/), a system package, or sources
# (FETCHCONTENT_SOURCE_DIR_SDL2) built statically.
set(SDL2_PREBUILT_DIR "" CACHE PATH "build directory of a static SDL2")
if(SDL2_PREBUILT_DIR)
  file(GLOB _sdl_cfg "${SDL2_PREBUILT_DIR}/include-config-*/SDL2")
  add_library(SDL2::SDL2-static STATIC IMPORTED)
  set_target_properties(SDL2::SDL2-static PROPERTIES IMPORTED_LOCATION "${SDL2_PREBUILT_DIR}/libSDL2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_PREBUILT_DIR}/include/SDL2;${_sdl_cfg}")
  add_library(SDL2::SDL2main STATIC IMPORTED)
  set_target_properties(SDL2::SDL2main PROPERTIES IMPORTED_LOCATION "${SDL2_PREBUILT_DIR}/libSDL2main.a")
  if(WIN32)
    set_property(TARGET SDL2::SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES
      m kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8)
  else()
    set_property(TARGET SDL2::SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES m dl pthread)
  endif()
  set(SDL2_FOUND TRUE)
else()
  find_package(SDL2 QUIET CONFIG)
endif()
if(NOT SDL2_FOUND)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2 URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
  FetchContent_MakeAvailable(SDL2)
endif()

set(RC_SOURCES "")
set(_games_decl "")
set(_games_list "")
set(_first TRUE)
foreach(_d IN LISTS RC_DIRS)
  get_filename_component(_d "${_d}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}/..")
  file(GLOB _src "${_d}/rc_*.c")
  list(APPEND RC_SOURCES ${_src})
  if(_first)
    list(APPEND RC_SOURCES "${_d}/interp_gen.c")   # same in every directory
    set(_first FALSE)
  endif()
  file(STRINGS "${_d}/rc_table.c" _line REGEX "^const RcGame rc_game_[A-Za-z0-9_]+")
  string(REGEX MATCH "rc_game_[A-Za-z0-9_]+" _sym "${_line}")
  string(APPEND _games_decl "extern const RcGame ${_sym};
")
  string(APPEND _games_list "&${_sym}, ")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/rc_games.c.in"
  "#include \"gb.h\"
${_games_decl}const RcGame *const rc_games[] = { ${_games_list}0 };
")
configure_file("${CMAKE_BINARY_DIR}/rc_games.c.in" "${CMAKE_BINARY_DIR}/rc_games.c" COPYONLY)
list(APPEND RC_SOURCES "${CMAKE_BINARY_DIR}/rc_games.c")
set(RUNTIME_SOURCES
  runtime/main.c runtime/mem.c runtime/hw.c runtime/ppu.c runtime/apu.c)

add_executable(${GAME_NAME} ${RUNTIME_SOURCES} ${RC_SOURCES})
target_include_directories(${GAME_NAME} PRIVATE runtime)
target_compile_definitions(${GAME_NAME} PRIVATE GB_CONFIG_NAME="${GAME_NAME}.ini")
if(TARGET SDL2::SDL2-static)
  target_link_libraries(${GAME_NAME} PRIVATE SDL2::SDL2main SDL2::SDL2-static)
else()
  target_link_libraries(${GAME_NAME} PRIVATE SDL2::SDL2main SDL2::SDL2)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${GAME_NAME} PRIVATE -Wall -Wno-unused-label -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-misleading-indentation)
  # the generated bank functions are huge: -O1 keeps compile times sane
  set_source_files_properties(${RC_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-w")
endif()
# Linux packages: run on glibc >= 2.34 even when built on a newer system
# (build SDL2 with the same flags: -std=gnu17 -include pc/cmake/glibc_compat.h)
option(GB_PORTABLE_GLIBC "link against old glibc symbol versions (Linux packages)" OFF)
if(GB_PORTABLE_GLIBC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_options(${GAME_NAME} PRIVATE -std=gnu17 -include ${CMAKE_SOURCE_DIR}/cmake/glibc_compat.h)
endif()
if(WIN32)
  set_target_properties(${GAME_NAME} PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()
