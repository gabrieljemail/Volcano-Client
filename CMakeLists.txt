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

# The exe's own icon and version info (a resource script); Windows only.
if(WIN32)
    # rc.exe isn't on PATH in a plain shell: look in the portable Windows SDK, then for LLVM's llvm-rc.
    file(GLOB _sdk_rc "C:/Apps/Windows SDK/bin/*/x64/rc.exe")
    find_program(VOLCANO_RC NAMES rc llvm-rc HINTS ${_sdk_rc} "C:/Apps/Clang+LLVM/bin" "C:/Program Files/LLVM/bin")
    if(VOLCANO_RC)
        set(CMAKE_RC_COMPILER "${VOLCANO_RC}" CACHE FILEPATH "Resource compiler" FORCE) # Overrides a stale bare "rc".
    endif()
    enable_language(RC)
endif()

# Where prebuilt copies of GLFW, ZLIB, OpenSSL, Asio and OpenAL live (the maintainer's Windows layout). Anything not
# found there comes from the system or is downloaded; see cmake/Dependencies.cmake. Set it empty to skip it.
set(_default_libs "")
if(EXISTS "C:/Apps/Libraries")
    set(_default_libs "C:/Apps/Libraries")
endif()
set(VOLCANO_LIBS_DIR "${_default_libs}" CACHE PATH "Prebuilt third-party libraries; empty uses the system or downloads them")
set(THIRD_PARTY "${CMAKE_SOURCE_DIR}/third_party") # ImGui, vk-bootstrap, VMA, nlohmann/json and stb, vendored.

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

# The DX11 backend is Windows-only (d3d11.h doesn't exist elsewhere) - keep
# its sources out of non-Windows builds even though the glob picks them up.
if(NOT WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "src/renderer/engine/d3d11/")
endif()

list(APPEND SOURCES
    "${THIRD_PARTY}/vk-bootstrap/src/VkBootstrap.cpp"
    "${THIRD_PARTY}/imgui/imgui.cpp"
    "${THIRD_PARTY}/imgui/imgui_demo.cpp"
    "${THIRD_PARTY}/imgui/imgui_draw.cpp"
    "${THIRD_PARTY}/imgui/imgui_tables.cpp"
    "${THIRD_PARTY}/imgui/imgui_widgets.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_glfw.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_vulkan.cpp"
)

if(WIN32)
    # ImGui's DX11 backend for the D3D11 render engine (AttachGUI TODO).
    list(APPEND SOURCES "${THIRD_PARTY}/imgui/backends/imgui_impl_dx11.cpp")
    list(APPEND SOURCES "src/platform/VolcanoClient.rc")
    set_source_files_properties("src/platform/VolcanoClient.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/logo.ico")
endif()

# Module scanning: the policy and variable must be set before the target exists
# (cmake_minimum_required is older than the policy's 3.28 default).
if(POLICY CMP0155)
    cmake_policy(SET CMP0155 NEW)
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
# Use FLTO.
set(INTERPROCEDURAL_OPTIMIZATION ON)
add_executable(${PROJECT_NAME} ${SOURCES})

# C++20 modules (src/modules/): the Volcano module, which today wraps
# Application.hpp. CMake 3.28+ scans sources for imports; the scan setting is
# read when the target is created (above), and the target property is repeated
# here so nothing depends on the policy default.
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_sources(${PROJECT_NAME} PRIVATE
    FILE_SET volcano_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_SOURCE_DIR}/src/modules"
    FILES
        "${CMAKE_SOURCE_DIR}/src/modules/Volcano.cppm"
        "${CMAKE_SOURCE_DIR}/src/modules/Volcano.Application.cppm")
# Headers that mods also include (GUIController.hpp, VulkanInit.hpp) import the
# module only in this build; the SDK build gets the plain header.
target_compile_definitions(${PROJECT_NAME} PRIVATE VOLCANO_MODULES)

# Mods resolve VC_EventHandler and the client's own classes from the exe:
# on Windows this emits an import library, on Linux it adds -rdynamic.
set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)

# Windows exports only what is marked dllexport, so before each link a script
# lists every Volcano:: symbol in our own objects into a .def file (see
# tools/gen_exports.py). The result is that mods can call any non-inline
# method or function; polymorphic bases and shared data also carry VC_API.
if(WIN32)
    find_program(LLVM_NM llvm-nm HINTS "C:/Apps/Clang+LLVM/bin" "C:/Program Files/LLVM/bin")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(EXPORTS_DEF "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.exports.def")
    add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_exports.py"
            "${LLVM_NM}" "${CMAKE_BINARY_DIR}/CMakeFiles/${PROJECT_NAME}.dir/src" "${CMAKE_SOURCE_DIR}/src" "${EXPORTS_DEF}" "${PROJECT_NAME}.exe"
            "${CMAKE_BINARY_DIR}/CMakeFiles/${PROJECT_NAME}.dir/third_party/imgui"
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
    "${THIRD_PARTY}/vk-bootstrap/src/VkBootstrap.cpp"
    "${THIRD_PARTY}/imgui/imgui.cpp"
    "${THIRD_PARTY}/imgui/imgui_demo.cpp"
    "${THIRD_PARTY}/imgui/imgui_draw.cpp"
    "${THIRD_PARTY}/imgui/imgui_tables.cpp"
    "${THIRD_PARTY}/imgui/imgui_widgets.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_glfw.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_vulkan.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_dx11.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# =============================================================================
# 3. Wire Up Manual Dependencies & Includes
# =============================================================================

# --- VULKAN (Automated SDK Discovery) ---
find_package(Vulkan REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

# --- THIRD-PARTY LIBRARIES (cmake/Dependencies.cmake) ---
# SYSTEM tells the compiler these are third-party headers, so warnings
# originating from inside them are suppressed even though -Wall -Wextra
# is enabled for our own code below.
include(cmake/Dependencies.cmake)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${VOLCANO_DEP_INCLUDES}
    "${THIRD_PARTY}"
    "${THIRD_PARTY}/imgui"
    "${THIRD_PARTY}/imgui/backends"
    "${THIRD_PARTY}/vk-bootstrap/src"
    "${THIRD_PARTY}/vma"
    "${THIRD_PARTY}/stb"
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${VOLCANO_DEP_LIBS})
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

# --- PLATFORM LIBRARIES (src/platform) ---
# DbgHelp symbolizes crash stack traces on Windows; -rdynamic exports symbol
# names so backtrace_symbols can on Linux.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE Dbghelp Winmm Winhttp)
elseif(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
endif()

# --- PINCEL (the UI renderer, built from its own checkout) ---
# Build it first (ninja in PINCEL_DIR, same build type as this). With a pincel.dll
# there the client links it and copies it next to the exe, so rebuilding Pincel is
# enough; without one it links the static libraries (rebuild the client to pick up changes).
get_filename_component(PINCEL_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../Pincel" ABSOLUTE)
set(PINCEL_DIR "${PINCEL_DEFAULT_DIR}" CACHE PATH "Pincel's source directory")
set(PINCEL_BUILD_DIR "${PINCEL_DIR}/build" CACHE PATH "Where Pincel's build put its library (and pincel.dll)")
if(WIN32)
    set(PINCEL_LIB_NAME pincel.lib)
    set(PINCEL_CORE_LIB_NAME pincel_core.lib)
else()
    set(PINCEL_LIB_NAME libpincel.a)
    set(PINCEL_CORE_LIB_NAME libpincel_core.a)
endif()
if(NOT EXISTS "${PINCEL_BUILD_DIR}/${PINCEL_LIB_NAME}")
    message(FATAL_ERROR "Pincel isn't built: ${PINCEL_BUILD_DIR}/${PINCEL_LIB_NAME} is missing. Build it in ${PINCEL_DIR} (cmake, then ninja), or set -DPINCEL_DIR=<path>.")
endif()
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${PINCEL_DIR}/src/pincel/cpp/include")
if(WIN32 AND EXISTS "${PINCEL_BUILD_DIR}/pincel.dll")
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
    if(WIN32)
        set(PINCEL_SYSTEM_LIBS advapi32 bcrypt kernel32 ntdll userenv ws2_32 synchronization)
    else()
        set(PINCEL_SYSTEM_LIBS ${CMAKE_DL_LIBS} pthread m)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${PINCEL_BUILD_DIR}/${PINCEL_LIB_NAME}"
        "${PINCEL_DIR}/cargo/${PINCEL_RUST_PROFILE}/${PINCEL_CORE_LIB_NAME}"
        ${PINCEL_SYSTEM_LIBS})
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

if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # -fms-compatibility forces Clang to emulate the MSVC compiler bugs/features needed for MSVC STL
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -ffast-math -fms-compatibility -fms-extensions
        -O3
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
        $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-DNDEBUG>
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
    # Likewise the Volcano module is built with -ffast-math, so these two cannot import it:
    # -UVOLCANO_MODULES makes the headers that would `import Volcano;` include Application.hpp instead.
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off;-UVOLCANO_MODULES" SKIP_PRECOMPILE_HEADERS ON
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

    # HLSL twins for the D3D11 backend: every .spv above also cross-compiles to
    # <name>.<stage>.hlsl next to it via SPIRV-Cross. Runs at build time (not
    # configure) so .spv changes retrigger it; DEPENDS on the .spv output lets
    # ninja chain glslang -> spirv-cross in one pass. Windows only: HLSL is
    # useless elsewhere. SM 4.0 is the floor every D3D_FEATURE_LEVEL_11_0 part
    # supports; 5.0 would also work but buys nothing yet.
    set(SHADER_HLSL "")
    if(WIN32)
        find_program(SPIRV_CROSS spirv-cross HINTS "C:/Apps/VulkanSDK/Bin" ENV VULKAN_SDK PATH_SUFFIXES Bin)
        if(SPIRV_CROSS)
            foreach(spv ${SHADER_BINARIES})
                string(REGEX REPLACE "\\.spv$" ".hlsl" hlsl "${spv}")
                add_custom_command(
                    OUTPUT ${hlsl}
                    COMMAND ${SPIRV_CROSS} ${spv} --hlsl --shader-model 40 --output ${hlsl}
                    DEPENDS ${spv}
                    COMMENT "Cross-compiling shader ${spv} to HLSL"
                    VERBATIM)
                list(APPEND SHADER_HLSL ${hlsl})
            endforeach()
        else()
            message(WARNING "spirv-cross not found; .hlsl shader twins will not be generated.")
        endif()
    endif()

    add_custom_target(Shaders DEPENDS ${SHADER_BINARIES} ${SHADER_HLSL})
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

# OpenSSL's .lib files from a prebuilt copy are import libraries for the shared-DLL build, so the DLLs have to sit
# beside the exe or the loader fails before main() runs. Dependencies.cmake lists them when it used a prebuilt copy.
if(VOLCANO_RUNTIME_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VOLCANO_RUNTIME_DLLS} $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()

# The Visual C++ runtime (the exe and OpenSSL both link the dynamic CRT). A clean PC without the redistributable would
# fail at launch with "VCRUNTIME140.dll was not found", so ship the DLLs beside the exe (app-local deployment). The
# Universal CRT (api-ms-win-crt-*) is part of Windows 10 and up, so it isn't needed. Skipped for VOLCANO_DEBUG_CHECKS,
# whose debug runtime isn't redistributable.
if(WIN32 AND NOT VOLCANO_DEBUG_CHECKS)
    set(_vc_redist_hints "")
    if(DEFINED ENV{VCToolsRedistDir})
        list(APPEND _vc_redist_hints "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
    endif()
    file(GLOB _vc_redist_dirs
        ${_vc_redist_hints}
        "C:/Apps/VS Build Tools/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
    list(SORT _vc_redist_dirs)
    list(REVERSE _vc_redist_dirs) # Newest first.
    set(VOLCANO_VC_RUNTIME_DLLS "")
    if(_vc_redist_dirs)
        list(GET _vc_redist_dirs 0 _vc_redist_dir)
        foreach(_dll vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll)
            if(EXISTS "${_vc_redist_dir}/${_dll}")
                list(APPEND VOLCANO_VC_RUNTIME_DLLS "${_vc_redist_dir}/${_dll}")
            endif()
        endforeach()
        message(STATUS "Visual C++ runtime: bundling DLLs from ${_vc_redist_dir}")
    else()
        message(WARNING "Visual C++ redistributable DLLs not found; the build will need the VC++ redistributable installed to run on other PCs.")
    endif()
    if(VOLCANO_VC_RUNTIME_DLLS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VOLCANO_VC_RUNTIME_DLLS} $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
    endif()
endif()

# =============================================================================
# 4b. volcano_core - the engine as a library, for programs other than the client (VC Editor)
# =============================================================================
# Two parts of the client are what other programs want:
#   Magma    the render engine (src/renderer/engine and the passes under it)
#   Conduit  the network client (src/network): the channel the server's world travels up through
# They are not yet separable from the rest of the client, so this library is every source except main(). A program
# that links it only pulls in the objects it actually references, so it gets Magma, Conduit and what they need, not
# the whole client. volcano_magma and volcano_conduit are the names to link: both are volcano_core today, and will
# become real libraries without changing who uses them.
#
# Opt-in, and the client above is not touched by it: the same sources are compiled a second time, with the same flags,
# into a target that is not part of `all`. Use a build directory of its own (the client builds in-source):
#   cmake -S . -B build-core -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DVOLCANO_BUILD_CORE=ON
#   cmake --build build-core --target volcano_core
# That also writes build-core/volcano_core.cmake, which VC Editor includes to get the library with its include paths,
# definitions and link libraries.
option(VOLCANO_BUILD_CORE "Define the volcano_core static library, and export it for VC Editor" OFF)
if(VOLCANO_BUILD_CORE)
    set(CORE_SOURCES ${SOURCES})
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/src/VolcanoClient\\.cpp$")

    add_library(volcano_core STATIC EXCLUDE_FROM_ALL ${CORE_SOURCES})

    # The client and this library each provide the `Volcano` module (the sources `import Volcano;`). That is fine as
    # long as no target links both.
    set_target_properties(volcano_core PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_sources(volcano_core PRIVATE
        FILE_SET volcano_core_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_SOURCE_DIR}/src/modules"
        FILES
            "${CMAKE_SOURCE_DIR}/src/modules/Volcano.cppm"
            "${CMAKE_SOURCE_DIR}/src/modules/Volcano.Application.cppm")
    # Only this library's own sources import the module. A program that links it includes Application.hpp instead.
    target_compile_definitions(volcano_core PRIVATE VOLCANO_MODULES)

    # What a program that includes Volcano's headers needs too, so these are PUBLIC.
    # VOLCANO_CLIENT_BUILD makes VC_API mean "export" instead of "import": without it, linking this library
    # statically into another program looks for __imp_ symbols that don't exist.
    target_compile_definitions(volcano_core PUBLIC VOLCANO_CLIENT_BUILD "ImDrawIdx=unsigned int")
    if(WIN32)
        target_compile_definitions(volcano_core PUBLIC
            _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH _HAS_VERBOSE_TRAP=0 _ENABLE_EXTENDED_ALIGNED_STORAGE)
    endif()

    target_include_directories(volcano_core PUBLIC src src/network src/renderer src/renderer/terrain
        src/renderer/entity src/renderer/gui)
    target_include_directories(volcano_core SYSTEM PUBLIC
        ${VOLCANO_DEP_INCLUDES}
        "${THIRD_PARTY}" "${THIRD_PARTY}/imgui" "${THIRD_PARTY}/imgui/backends"
        "${THIRD_PARTY}/vk-bootstrap/src" "${THIRD_PARTY}/vma" "${THIRD_PARTY}/stb"
        "${PINCEL_DIR}/src/pincel/cpp/include")
    target_link_libraries(volcano_core PUBLIC Vulkan::Vulkan ${VOLCANO_DEP_LIBS})
    if(WIN32)
        target_link_libraries(volcano_core PUBLIC Dbghelp Winmm Winhttp)
    elseif(UNIX)
        target_link_libraries(volcano_core PUBLIC ${CMAKE_DL_LIBS})
    endif()

    # Pincel, the same way as the client above.
    set(CORE_RUNTIME_DLLS ${VOLCANO_RUNTIME_DLLS})
    if(WIN32 AND EXISTS "${PINCEL_BUILD_DIR}/pincel.dll")
        target_compile_definitions(volcano_core PUBLIC PINCEL_SHARED)
        target_link_libraries(volcano_core PUBLIC "${PINCEL_BUILD_DIR}/pincel.lib")
        list(APPEND CORE_RUNTIME_DLLS "${PINCEL_BUILD_DIR}/pincel.dll")
    else()
        target_link_libraries(volcano_core PUBLIC
            "${PINCEL_BUILD_DIR}/${PINCEL_LIB_NAME}"
            "${PINCEL_DIR}/cargo/${PINCEL_RUST_PROFILE}/${PINCEL_CORE_LIB_NAME}"
            ${PINCEL_SYSTEM_LIBS})
    endif()

    # The client's own flags, for this library's own sources only (PRIVATE).
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(volcano_core PRIVATE
            -Wall -Wextra -ffast-math -fms-compatibility -fms-extensions -O3 -g -gcodeview)
        if(NOT VOLCANO_DEBUG_CHECKS)
            target_compile_definitions(volcano_core PRIVATE NDEBUG)
        endif()
    else()
        target_compile_options(volcano_core PRIVATE
            -Wall -Wextra -pthread -ffast-math
            $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-DNDEBUG>
            $<$<NOT:$<CONFIG:Release>>:-g> $<$<NOT:$<CONFIG:Release>>:-Og> $<$<NOT:$<CONFIG:Release>>:-march=native>)
    endif()
    target_precompile_headers(volcano_core PRIVATE <vulkan/vulkan.h> <glm/glm.hpp>)
    # Per-file settings (-fno-fast-math for TickLoop and InteractionManager, -w for third-party sources) are source
    # file properties above, so they already apply here.

    # The names other projects link.
    add_library(volcano_magma INTERFACE)
    target_link_libraries(volcano_magma INTERFACE volcano_core)
    add_library(volcano_conduit INTERFACE)
    target_link_libraries(volcano_conduit INTERFACE volcano_core)

    # Export for VC Editor. Only with prebuilt libraries (VOLCANO_LIBS_DIR): libraries fetched at configure time
    # are targets of this project that cannot be exported.
    if(VOLCANO_LIBS_DIR)
        export(TARGETS volcano_core volcano_magma volcano_conduit NAMESPACE volcano::
            FILE "${CMAKE_BINARY_DIR}/volcano_core-targets.cmake")
        file(WRITE "${CMAKE_BINARY_DIR}/volcano_core.cmake"
            "# Written by Volcano Client's CMake (-DVOLCANO_BUILD_CORE=ON). Include it from another project.\n"
            "# That project must find Vulkan first (find_package(Vulkan REQUIRED)): the library links Vulkan::Vulkan.\n"
            "include(\"\${CMAKE_CURRENT_LIST_DIR}/volcano_core-targets.cmake\")\n"
            "set(VOLCANO_CORE_SOURCE_DIR \"${CMAKE_SOURCE_DIR}\")\n"
            "# Volcano loads resources/ relative to its working directory.\n"
            "set(VOLCANO_CORE_RESOURCES_DIR \"${CMAKE_SOURCE_DIR}/resources\")\n"
            "# Copy these next to any executable that links volcano::volcano_core.\n"
            "set(VOLCANO_CORE_RUNTIME_DLLS \"${CORE_RUNTIME_DLLS}\")\n")
    else()
        message(WARNING "volcano_core is not exported: VOLCANO_LIBS_DIR is empty, so some dependencies are built by this project.")
    endif()
endif()

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
        $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<NOT:$<CONFIG:Release>>:-g> $<$<NOT:$<CONFIG:Release>>:-gcodeview> $<$<NOT:$<CONFIG:Release>>:-O0>
    )
else()
    target_compile_options(NetDiag PRIVATE
        -pthread
        $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-DNDEBUG>
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
