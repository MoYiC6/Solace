cmake_minimum_required(VERSION 3.25)
project(SolaceReconstruction VERSION 0.2.0 LANGUAGES C CXX)
if(NOT WIN32)
  message(FATAL_ERROR "This recovery target requires Windows and the Windows SDK.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
include(CTest)

# Dependencies are pinned, local, and built from source. Configuration never downloads.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE third_party)

file(GLOB LUA_SOURCES CONFIGURE_DEPENDS third_party/lua-5.4.7/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(solace_lua STATIC ${LUA_SOURCES})
target_sources(solace_lua PRIVATE src/lua_utf8_io.c)
# Preserve Lua's official file loader/searcher; only adapt Windows UTF-8 paths.
foreach(lua_file lauxlib loadlib liolib)
  set_property(SOURCE "third_party/lua-5.4.7/src/${lua_file}.c" APPEND
               PROPERTY COMPILE_DEFINITIONS SOLACE_LUA_UTF8_IO_REDIRECT)
  if(MSVC)
    set_property(SOURCE "third_party/lua-5.4.7/src/${lua_file}.c" APPEND
                 PROPERTY COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/lua_utf8_io.h")
  else()
    set_property(SOURCE "third_party/lua-5.4.7/src/${lua_file}.c" APPEND
                 PROPERTY COMPILE_OPTIONS "-include" "${CMAKE_CURRENT_SOURCE_DIR}/src/lua_utf8_io.h")
  endif()
endforeach()
set_target_properties(solace_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(solace_lua PUBLIC third_party/lua-5.4.7/src)
if(WIN32)
  target_compile_definitions(solace_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(solace_lua PUBLIC m ${CMAKE_DL_LIBS})
endif()

set(ASSET_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/manifest.json")
set(BINARY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/binary/manifest.json")
file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS resources/sources/* resources/binary/*)
set(ASSET_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/solace/assets_generated.hpp")
add_custom_command(OUTPUT "${ASSET_HEADER}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_assets.py"
          --manifest "${ASSET_MANIFEST}" --binary-manifest "${BINARY_MANIFEST}" --output "${ASSET_HEADER}"
  DEPENDS tools/generate_assets.py "${ASSET_MANIFEST}" "${BINARY_MANIFEST}" ${ASSET_FILES}
  VERBATIM)

add_library(solace_core STATIC
  src/assets.cpp src/config.cpp src/bootstrap.cpp src/script.cpp src/script_utils.cpp
  src/script_services.cpp src/script_session.cpp src/script_callbacks.cpp
  src/script_math.cpp src/script_draw_values.cpp src/shaders.cpp "${ASSET_HEADER}")
set_target_properties(solace_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(solace_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(solace_core PUBLIC nlohmann_json solace_lua d3dcompiler dxguid Crypt32 User32)
if(MSVC)
  target_compile_options(solace_core PRIVATE /W4 /permissive- /utf-8 /fp:strict)
else()
  target_compile_options(solace_core PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

add_executable(solace-tool src/main.cpp)
target_link_libraries(solace-tool PRIVATE solace_core)
if(MSVC)
  target_compile_options(solace-tool PRIVATE /utf-8)
endif()

add_library(SolaceCore SHARED src/api.cpp)
target_link_libraries(SolaceCore PRIVATE solace_core)
target_compile_definitions(SolaceCore PRIVATE SOLACE_CORE_EXPORTS)
target_include_directories(SolaceCore PUBLIC include)
if(MSVC)
  target_compile_options(SolaceCore PRIVATE /W4 /utf-8)
endif()

if(BUILD_TESTING)
  add_executable(native_float_probe tests/native_float_probe.cpp)
  if(MSVC)
    target_compile_options(native_float_probe PRIVATE /Od /fp:strict /utf-8)
  endif()
  add_executable(api_tests tests/api_tests.cpp)
  target_link_libraries(api_tests PRIVATE SolaceCore)
  add_test(NAME dll_api COMMAND api_tests)
  foreach(module config assets bootstrap script script_utils script_math script_draw_values
                 script_callbacks script_services script_session lua_utf8_io shaders)
    add_executable(${module}_tests tests/${module}_tests.cpp)
    target_link_libraries(${module}_tests PRIVATE solace_core)
    if(MSVC)
      target_compile_options(${module}_tests PRIVATE /utf-8 /fp:strict)
    endif()
    add_test(NAME ${module} COMMAND ${module}_tests)
  endforeach()
  add_test(NAME script_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_cli_session.py" --tool "$<TARGET_FILE:solace-tool>")
endif()

install(TARGETS SolaceCore solace_core solace_lua solace-tool
        RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES third_party/nlohmann/json.hpp DESTINATION include/nlohmann)
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION licenses/nlohmann-json)
install(FILES third_party/lua-5.4.7/src/lua.hpp third_party/lua-5.4.7/src/lua.h
              third_party/lua-5.4.7/src/lauxlib.h third_party/lua-5.4.7/src/lualib.h
              third_party/lua-5.4.7/src/luaconf.h DESTINATION include)
install(FILES third_party/lua-5.4.7/doc/readme.html DESTINATION licenses/lua)
