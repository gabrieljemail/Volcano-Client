cmake_minimum_required(VERSION 3.20)

# =============================================================================
# 1. Critical Toolchain Interception Macros (MUST BE AT THE VERY TOP)
# =============================================================================
if(WIN32)
    # Bypasses the Microsoft MSVC version gatekeeping checks
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    # Fixes '__builtin_verbose_trap' errors on Clang < 19
    add_compile_definitions(_HAS_VERBOSE_TRAP=0)
    # Ensures standard alignment features don't throw macro syntax crashes
    add_compile_definitions(_ENABLE_EXTENDED_ALIGNED_STORAGE)
endif()

# Debug here exists for symbols, not for a slow runtime: it is optimized and uses the release CRT and STL, like Release.
# The debug CRT's checked heap and iterators cost several times the speed in allocation-heavy code and skew every
# measurement. ON brings them back, with assertions, for chasing memory bugs (build Pincel the same way).
option(VOLCANO_CONSOLE "Keep a console window open on Windows" OFF)
option(VOLCANO_DEBUG_CHECKS "Debug builds use the debug CRT and STL and keep assertions" OFF)
if(WIN32 AND NOT VOLCANO_DEBUG_CHECKS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

project(VolcanoClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ninja is single-config, so this is what distinguishes the normal in-source
# dev build (Debug, the default) from an out-of-source `build-release/`
# configured with -DCMAKE_BUILD_TYPE=Release — see release.ps1.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

# =============================================================================
# 2. Gather Sources & Define Target
# =============================================================================
# CONFIGURE_DEPENDS re-checks the glob on every build, so adding a new .cpp
# (e.g. a render pass under src/renderer/engine/passes/) is picked up by a
# plain `ninja` run instead of needing a manual CMake re-configure first.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(APPEND SOURCES
    "C:/Apps/Libraries/vk-bootstrap/src/VkBootstrap.cpp"
    "C:/Apps/Libraries/ImGui/imgui.cpp"
    "C:/Apps/Libraries/ImGui/imgui_demo.cpp"
    "C:/Apps/Libraries/ImGui/imgui_draw.cpp"
    "C:/Apps/Libraries/ImGui/imgui_tables.cpp"
    "C:/Apps/Libraries/ImGui/imgui_widgets.cpp"
    "C:/Apps/Libraries/ImGui/backends/imgui_impl_glfw.cpp"
    "C:/Apps/Libraries/ImGui/backends/imgui_impl_vulkan.cpp"
)

add_executable(${PROJECT_NAME} ${SOURCES})

# Mods resolve VC_EventHandler and the client's own classes from the exe:
# on Windows this emits an import library, on Linux it adds -rdynamic.
set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)

# Windows exports only what is marked dllexport, so before each link a script
# lists every Volcano:: symbol in our own objects into a .def file (see
# tools/gen_exports.py). The result is that mods can call any non-inline
# method or function; polymorphic bases and shared data also carry VC_API.
if(WIN32)
    find_program(LLVM_NM llvm-nm HINTS "C:/Apps/Clang+LLVM/bin")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(EXPORTS_DEF "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.exports.def")
    add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_exports.py"
            "${LLVM_NM}" "${CMAKE_BINARY_DIR}/CMakeFiles/${PROJECT_NAME}.dir/src" "${CMAKE_SOURCE_DIR}/src" "${EXPORTS_DEF}" "${PROJECT_NAME}.exe"
            "${CMAKE_BINARY_DIR}/CMakeFiles/${PROJECT_NAME}.dir/C_/Apps/Libraries/ImGui"
        COMMENT "Listing exported symbols"
        VERBATIM)
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:/DEF:${EXPORTS_DEF}")
endif()

# 32-bit ImGui indices: GUIController::AddTextShadows appends vertices to
# draw lists, which could overflow 16-bit indices on a busy frame.
target_compile_definitions(${PROJECT_NAME} PRIVATE "ImDrawIdx=unsigned int")
# Marks the client side of the mod API (VC_API / VC_HOST_API export, not import).
target_compile_definitions(${PROJECT_NAME} PRIVATE VOLCANO_CLIENT_BUILD)

# Precompiled headers: vulkan.h and glm.hpp are both large, near-universally
# included across src/renderer, and stable (no per-TU #define gates the way
# vk_mem_alloc.h/stb_image.h need VMA_IMPLEMENTATION/STB_IMAGE_IMPLEMENTATION
# in exactly one .cpp — those two are deliberately left out of this list,
# since precompiling them would bake in whichever #define state happened to
# be active first and silently break the single file that needs the other
# state). Cuts redundant parsing across ~150 translation units, which
# matters more than usual on this machine's tight RAM budget.
target_precompile_headers(${PROJECT_NAME} PRIVATE
    <vulkan/vulkan.h>
    <glm/glm.hpp>
)

# Third-party sources compiled directly into this target: silence their
# warnings too, since -Wall -Wextra below applies per translation unit
# and SYSTEM include dirs don't cover a file's own warnings.
set_source_files_properties(
    "C:/Apps/Libraries/vk-bootstrap/src/VkBootstrap.cpp"
    "C:/Apps/Libraries/ImGui/imgui.cpp"
    "C:/Apps/Libraries/ImGui/imgui_demo.cpp"
    "C:/Apps/Libraries/ImGui/imgui_draw.cpp"
    "C:/Apps/Libraries/ImGui/imgui_tables.cpp"
    "C:/Apps/Libraries/ImGui/imgui_widgets.cpp"
    "C:/Apps/Libraries/ImGui/backends/imgui_impl_glfw.cpp"
    "C:/Apps/Libraries/ImGui/backends/imgui_impl_vulkan.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# =============================================================================
# 3. Wire Up Manual Dependencies & Includes
# =============================================================================

# --- VULKAN (Automated SDK Discovery) ---
find_package(Vulkan REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

# --- MANUAL SYSTEM HEADERS ---
# SYSTEM tells the compiler these are third-party headers, so warnings
# originating from inside them are suppressed even though -Wall -Wextra
# is enabled for our own code below.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    "C:/Apps/Libraries/GLFW/include"
    "C:/Apps/Libraries/ZLIB"
    "C:/Apps/Libraries/OpenSSL/include"
    "C:/Apps/Libraries/Asio/include"
    "C:/Apps/Libraries/STB"
    "C:/Apps/Libraries/ImGui"
    "C:/Apps/Libraries/ImGui/backends"
)

# --- OPENAL ---
if(WIN32)
    target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
        "C:/Apps/Libraries/OpenAL/include"
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OPENAL_LIBRARY "C:/Apps/Libraries/OpenAL/libs/Win64/OpenAL32.lib")
    else()
        set(OPENAL_LIBRARY "C:/Apps/Libraries/OpenAL/libs/Win32/OpenAL32.lib")
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${OPENAL_LIBRARY}"
    )
elseif(UNIX)
    find_package(OpenAL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenAL::OpenAL ${CMAKE_DL_LIBS})
endif()

# --- PLATFORM LIBRARIES (src/platform) ---
# DbgHelp symbolizes crash stack traces on Windows; -rdynamic exports symbol
# names so backtrace_symbols can on Linux.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE Dbghelp Winmm Winhttp)
elseif(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
endif()

# --- MANUAL LINK LIBRARIES ---
target_link_libraries(${PROJECT_NAME} PRIVATE
    "C:/Apps/Libraries/GLFW/lib-vc2026/glfw3.lib"
    "C:/Apps/Libraries/ZLIB/zlib.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libcrypto.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libssl.lib"
)

# --- PINCEL (the UI renderer, built from its own checkout) ---
# Build it first (ninja in PINCEL_DIR, same build type as this). With a pincel.dll
# there the client links it and copies it next to the exe, so rebuilding Pincel is
# enough; without one it links the static libraries (rebuild the client to pick up changes).
get_filename_component(PINCEL_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../Pincel" ABSOLUTE)
set(PINCEL_DIR "${PINCEL_DEFAULT_DIR}" CACHE PATH "Pincel's source directory")
set(PINCEL_BUILD_DIR "${PINCEL_DIR}/build" CACHE PATH "Where Pincel's build put pincel.lib and pincel.dll")
if(NOT EXISTS "${PINCEL_BUILD_DIR}/pincel.lib")
    message(FATAL_ERROR "Pincel isn't built: ${PINCEL_BUILD_DIR}/pincel.lib is missing. Run ninja in ${PINCEL_DIR}, or set -DPINCEL_DIR=<path>.")
endif()
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${PINCEL_DIR}/src/pincel/cpp/include")
if(EXISTS "${PINCEL_BUILD_DIR}/pincel.dll")
    message(STATUS "Pincel: linking ${PINCEL_BUILD_DIR}/pincel.dll")
    target_compile_definitions(${PROJECT_NAME} PRIVATE PINCEL_SHARED)
    target_link_libraries(${PROJECT_NAME} PRIVATE "${PINCEL_BUILD_DIR}/pincel.lib")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PINCEL_BUILD_DIR}/pincel.dll" $<TARGET_FILE_DIR:${PROJECT_NAME}>)
else()
    message(STATUS "Pincel: linking the static libraries in ${PINCEL_BUILD_DIR}")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(PINCEL_RUST_PROFILE debug)
    else()
        set(PINCEL_RUST_PROFILE release)
    endif()
    # Rust's std needs these when linked from C++ (see Pincel's CMakeLists.txt).
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${PINCEL_BUILD_DIR}/pincel.lib"
        "${PINCEL_DIR}/cargo/${PINCEL_RUST_PROFILE}/pincel_core.lib"
        advapi32 bcrypt kernel32 ntdll userenv ws2_32 synchronization)
endif()

# =============================================================================
# 4. Project Specific Include Directories & Compiler Flags
# =============================================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/network
    src/renderer
    src/renderer/terrain
    src/renderer/entity
    src/renderer/gui
)

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    lib/vk-bootstrap/src
    lib/vulkanmemory/include
    lib
)

if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # -fms-compatibility forces Clang to emulate the MSVC compiler bugs/features needed for MSVC STL
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -ffast-math -fms-compatibility -fms-extensions
        -O2
        -g -gcodeview
    )
    # NDEBUG in every config unless VOLCANO_DEBUG_CHECKS asks for the checks (see the top of this file).
    if(NOT VOLCANO_DEBUG_CHECKS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE NDEBUG)
    endif()
    # Every config keeps debug info (no effect on speed) so crash reports name
    # real functions; the PDB just has to sit next to the exe.
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,/DEBUG)
    # A window app, not a console one: no black terminal behind the game. OS::AttachParentConsole still prints
    # logs into the terminal it was started from. -DVOLCANO_CONSOLE=ON brings the always-on console back.
    if(NOT VOLCANO_CONSOLE)
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup)
    endif()
else()
    # -march=native only makes sense for a Debug build run on the same
    # machine that built it — a Release build is meant to run on other
    # devices, so it stays portable instead.
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -pthread -ffast-math
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<NOT:$<CONFIG:Release>>:-g> $<$<NOT:$<CONFIG:Release>>:-Og> $<$<NOT:$<CONFIG:Release>>:-march=native>
    )
endif()

# Player physics and block/entity interaction have to match vanilla's exact
# IEEE arithmetic (the server re-simulates the same collision and rejects
# positions that differ by even a float ulp), so they're built without the
# project-wide -ffast-math — which lets the optimizer reassociate and assume
# no infinities/NaNs, and is undefined behavior for code that touches either.
set_source_files_properties(
    src/TickLoop.cpp
    src/interaction/InteractionManager.cpp
    # The PCH is built with -ffast-math, and clang rejects mixing FP modes.
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off" SKIP_PRECOMPILE_HEADERS ON
)

# Only meaningful for an out-of-source build. The root tree configures
# in-source (CMAKE_BINARY_DIR == CMAKE_SOURCE_DIR), where this would copy
# resources/ onto itself — a no-op that cmake -E copy_directory actually
# errors out on, failing the build after a successful link.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources ${CMAKE_BINARY_DIR}/resources
    )
endif()

# Shaders: the client loads precompiled resources/shaders/<name>.spv, so each
# GLSL source is compiled next to itself whenever it changes.
find_program(GLSLANG_VALIDATOR glslangValidator HINTS "C:/Apps/VulkanSDK/Bin" ENV VULKAN_SDK PATH_SUFFIXES Bin)
if(GLSLANG_VALIDATOR)
    file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/resources/shaders/*.vert"
        "${CMAKE_SOURCE_DIR}/resources/shaders/*.frag"
        "${CMAKE_SOURCE_DIR}/resources/shaders/*.comp")
    set(SHADER_BINARIES "")
    foreach(shader ${SHADER_SOURCES})
        set(binary "${shader}.spv")
        add_custom_command(
            OUTPUT ${binary}
            COMMAND ${GLSLANG_VALIDATOR} -V ${shader} -o ${binary}
            DEPENDS ${shader}
            COMMENT "Compiling shader ${shader}"
            VERBATIM)
        list(APPEND SHADER_BINARIES ${binary})
    endforeach()
    # Variants: one source, compiled again with a define under another name -
    # terrain_shadow.* is terrain.* plus shadow receiving, so the plain
    # terrain shaders never pay for it.
    function(add_shader_variant source name define)
        set(src "${CMAKE_SOURCE_DIR}/resources/shaders/${source}")
        set(out "${CMAKE_SOURCE_DIR}/resources/shaders/${name}.spv")
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${GLSLANG_VALIDATOR} -V -D${define} ${src} -o ${out}
            DEPENDS ${src}
            COMMENT "Compiling shader variant ${name}"
            VERBATIM)
        set(SHADER_BINARIES ${SHADER_BINARIES} ${out} PARENT_SCOPE)
    endfunction()
    add_shader_variant(terrain.vert terrain_shadow.vert SHADOWS)
    add_shader_variant(terrain_indirect.vert terrain_shadow_indirect.vert SHADOWS)
    add_shader_variant(terrain.frag terrain_shadow.frag SHADOWS)
    add_shader_variant(misc.vert misc_shadow.vert SHADOWS)
    add_shader_variant(misc_indirect.vert misc_shadow_indirect.vert SHADOWS)
    add_shader_variant(misc.frag misc_shadow.frag SHADOWS)
    add_shader_variant(entity.vert entity_shadow.vert SHADOWS)
    add_shader_variant(entity.frag entity_shadow.frag SHADOWS)
    add_shader_variant(entity_textured.vert entity_textured_shadow.vert SHADOWS)
    add_shader_variant(entity_textured.frag entity_textured_shadow.frag SHADOWS)

    add_custom_target(Shaders DEPENDS ${SHADER_BINARIES})
    add_dependencies(${PROJECT_NAME} Shaders)
else()
    message(WARNING "glslangValidator not found; shaders will not be recompiled.")
endif()

# Example mods (sdk/examples): off by default, built into mods/.
option(VOLCANO_BUILD_EXAMPLES "Build the example mods in sdk/examples into mods/" OFF)
if(VOLCANO_BUILD_EXAMPLES)
    include(sdk/VolcanoMod.cmake)
    volcano_add_mod(hello_mod SOURCES sdk/examples/hello_mod/hello_mod.cpp)
endif()

# OpenSSL's libcrypto.lib/libssl.lib (linked above) are import libraries for
# the shared-DLL ("MD") build of OpenSSL, not static libraries — the actual
# code lives in libcrypto-4-x64.dll/libssl-4-x64.dll under OpenSSL's bin/,
# which isn't on PATH. Without a copy alongside the exe, the loader fails
# before main() runs (DLL not found), which is unrelated to anything the
# app itself does at runtime.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/Apps/Libraries/OpenSSL/bin/libcrypto-4-x64.dll"
        "C:/Apps/Libraries/OpenSSL/bin/libssl-4-x64.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
)

# =============================================================================
# 5. NetDiag — standalone network-protocol diagnostic (no Vulkan/GLFW/GUI),
#    for iterating on the Configuration/Play packet exchange against a real
#    server without needing the full client. Not part of the shipped app.
# =============================================================================
#[[
add_executable(NetDiag
    tools/netdiag_main.cpp
    src/Logger.cpp
    src/Config.cpp
    src/PlayerAttributes.cpp
    src/network/Connection.cpp
    src/network/NBT.cpp
    src/network/NetworkClient.cpp
    src/network/TextComponent.cpp
    src/renderer/terrain/ChunkParser.cpp
    src/renderer/terrain/models/BlockRegistry.cpp
    src/renderer/entity/models/EntityRegistry.cpp
    src/renderer/gui/models/Chat.cpp
)

target_include_directories(NetDiag PRIVATE
    src
    src/network
    src/renderer
    src/renderer/terrain
    src/renderer/entity
    src/renderer/gui
)

target_include_directories(NetDiag SYSTEM PRIVATE
    "C:/Apps/Libraries/GLFW/include"
    "C:/Apps/Libraries/ZLIB"
    "C:/Apps/Libraries/OpenSSL/include"
    "C:/Apps/Libraries/Asio/include"
    lib
)

target_link_libraries(NetDiag PRIVATE
    Vulkan::Vulkan
    "C:/Apps/Libraries/GLFW/lib-vc2026/glfw3.lib"
    "C:/Apps/Libraries/ZLIB/zlib.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libcrypto.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libssl.lib"
)

if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(NetDiag PRIVATE
        -fms-compatibility -fms-extensions
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<NOT:$<CONFIG:Release>>:-g> $<$<NOT:$<CONFIG:Release>>:-gcodeview> $<$<NOT:$<CONFIG:Release>>:-O0>
    )
else()
    target_compile_options(NetDiag PRIVATE
        -pthread
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<NOT:$<CONFIG:Release>>:-g> $<$<NOT:$<CONFIG:Release>>:-Og>
    )
endif()

add_custom_command(TARGET NetDiag POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/Apps/Libraries/OpenSSL/bin/libcrypto-4-x64.dll"
        "C:/Apps/Libraries/OpenSSL/bin/libssl-4-x64.dll"
        $<TARGET_FILE_DIR:NetDiag>
)
]]
