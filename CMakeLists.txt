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

project(VolcanoClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()

# =============================================================================
# 2. Gather Sources & Define Target
# =============================================================================
file(GLOB_RECURSE SOURCES "src/*.cpp")
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

# --- MANUAL LINK LIBRARIES ---
target_link_libraries(${PROJECT_NAME} PRIVATE
    "C:/Apps/Libraries/GLFW/lib-vc2026/glfw3.lib"
    "C:/Apps/Libraries/ZLIB/zlib.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libcrypto.lib"
    "C:/Apps/Libraries/OpenSSL/lib/VC/x64/MD/libssl.lib"
)

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
        -Wall -Wextra -g -gcodeview -O0 -ffast-math
        -fms-compatibility
        -fms-extensions
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pthread -g -Og -march=native -ffast-math)
endif()

# Only meaningful for an out-of-source build. The root tree configures
# in-source (CMAKE_BINARY_DIR == CMAKE_SOURCE_DIR), where this would copy
# resources/ onto itself — a no-op that cmake -E copy_directory actually
# errors out on, failing the build after a successful link.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources ${CMAKE_BINARY_DIR}/resources
    )
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
add_executable(NetDiag
    tools/netdiag_main.cpp
    src/Logger.cpp
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
    target_compile_options(NetDiag PRIVATE -g -gcodeview -O0 -fms-compatibility -fms-extensions)
else()
    target_compile_options(NetDiag PRIVATE -pthread -g -Og)
endif()

add_custom_command(TARGET NetDiag POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "C:/Apps/Libraries/OpenSSL/bin/libcrypto-4-x64.dll"
        "C:/Apps/Libraries/OpenSSL/bin/libssl-4-x64.dll"
        $<TARGET_FILE_DIR:NetDiag>
)
