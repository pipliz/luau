cmake_minimum_required(VERSION 3.24)
project(LuauNative LANGUAGES C CXX)

# This entry point packages upstream sources without modifying the VM/compiler.
get_filename_component(LUAU_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)
set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory("${LUAU_ROOT}" upstream EXCLUDE_FROM_ALL)

# Reuse upstream's explicit source lists, avoiding duplicated globs and archive
# dead stripping. CodeGen, Analysis, Require and CLI are deliberately absent.
add_library(luau SHARED free.cpp)
foreach(component Common Ast Bytecode Compiler VM)
    get_target_property(component_sources Luau.${component} SOURCES)
    foreach(source IN LISTS component_sources)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${LUAU_ROOT}/${source}")
        endif()
        target_sources(luau PRIVATE "${source}")
        get_source_file_property(source_flags "${source}" DIRECTORY "${LUAU_ROOT}" COMPILE_FLAGS)
        if(source_flags)
            set_source_files_properties("${source}" PROPERTIES COMPILE_FLAGS "${source_flags}")
        endif()
    endforeach()
    target_include_directories(luau PRIVATE "${LUAU_ROOT}/${component}/include")
    get_target_property(component_options Luau.${component} COMPILE_OPTIONS)
    if(component_options)
        target_compile_options(luau PRIVATE ${component_options})
    endif()
endforeach()
target_compile_features(luau PRIVATE cxx_std_17)
target_compile_definitions(luau PRIVATE LUA_USE_LONGJMP=1 LUA_VECTOR_SIZE=3 LUA_VECTOR_DOUBLE=0)
if(MSVC)
    set(api_export "extern \"C\" __declspec(dllexport)")
else()
    set(api_export "extern \"C\" __attribute__((visibility(\"default\")))")
endif()
target_compile_definitions(luau PRIVATE "LUA_API=${api_export}" "LUACODE_API=${api_export}")
set_target_properties(luau PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set_target_properties(luau PROPERTIES INSTALL_NAME_DIR "@rpath")
endif()
if(UNIX AND NOT APPLE)
    target_link_options(luau PRIVATE "-Wl,--no-undefined")
endif()

install(TARGETS luau RUNTIME DESTINATION lib LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES
    "${LUAU_ROOT}/VM/include/lua.h"
    "${LUAU_ROOT}/VM/include/lualib.h"
    "${LUAU_ROOT}/VM/include/luaconf.h"
    "${LUAU_ROOT}/Compiler/include/luacode.h"
    luau_native.h DESTINATION include)
install(FILES "${LUAU_ROOT}/LICENSE.txt" "${LUAU_ROOT}/lua_LICENSE.txt" DESTINATION licenses)
install(FILES README.md version.json DESTINATION .)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/toolchain-$<CONFIG>.json" CONTENT
    "{\"compiler\":\"${CMAKE_CXX_COMPILER_ID}\",\"compiler_version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"cmake_version\":\"${CMAKE_VERSION}\",\"configuration\":\"$<CONFIG>\"}\n")

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME native-abi-smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/smoke.py" "$<TARGET_FILE:luau>")
endif()
