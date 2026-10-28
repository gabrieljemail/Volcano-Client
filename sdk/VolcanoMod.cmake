# Builds a mod DLL/.so against the client's classes. Usage:
#
#   include(sdk/VolcanoMod.cmake)
#   volcano_add_mod(my_mod SOURCES my_mod.cpp)
#
# Inside the client's own CMake tree (VolcanoClient is a target) it reuses the
# client's include paths. From elsewhere, set VOLCANO_CLIENT_DIR to the client
# folder (the one holding VolcanoClient.lib and src/) before including this;
# you must then also make Vulkan, GLFW, ImGui and glm headers visible
# yourself, using the same versions the client was built with.
#
# The result goes to <client>/mods/ so the client loads it on next launch.
# A mod must be built like the client: clang, the MD runtime, C++20.

function(volcano_add_mod name)
    cmake_parse_arguments(MOD "" "" "SOURCES" ${ARGN})

    if(TARGET VolcanoClient)
        set(client_dir "${CMAKE_SOURCE_DIR}")
        set(client_lib "${CMAKE_BINARY_DIR}/VolcanoClient.lib")
    else()
        set(client_dir "${VOLCANO_CLIENT_DIR}")
        set(client_lib "${VOLCANO_CLIENT_DIR}/VolcanoClient.lib")
    endif()

    add_library(${name} SHARED ${MOD_SOURCES})
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${client_dir}/mods"
        LIBRARY_OUTPUT_DIRECTORY "${client_dir}/mods")

    target_include_directories(${name} PRIVATE
        "${client_dir}/src" "${client_dir}/src/network" "${client_dir}/src/renderer"
        "${client_dir}/src/renderer/terrain" "${client_dir}/src/renderer/entity" "${client_dir}/src/renderer/gui")
    if(TARGET VolcanoClient)
        # Third-party headers (ImGui, GLFW, glm, Vulkan...) exactly as the client sees them.
        target_include_directories(${name} SYSTEM PRIVATE $<TARGET_PROPERTY:VolcanoClient,INCLUDE_DIRECTORIES>)
        target_link_libraries(${name} PRIVATE Vulkan::Vulkan)
        add_dependencies(${name} VolcanoClient)
    endif()

    # Must match the client's own definitions: ImDrawIdx changes ImGui's ABI.
    target_compile_definitions(${name} PRIVATE
        "ImDrawIdx=unsigned int" _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH _HAS_VERBOSE_TRAP=0
        _ENABLE_EXTENDED_ALIGNED_STORAGE)

    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -ffast-math -fms-compatibility -fms-extensions)
        # The client's exports (every Volcano:: function) come through its import library.
        target_link_libraries(${name} PRIVATE "${client_lib}")
    endif()
endfunction()
