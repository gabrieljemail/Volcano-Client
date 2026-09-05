cmake_minimum_required(VERSION 3.20)
project(VolcanoClient LANGUAGES CXX)

# -----------------------------------------------------------------------------
# 1. Compiler & Standard Configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Clang and LLD for faster linking and better optimization
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")

# Enable Interprocedural Optimization (LTO) - Critical for HD 530 performance
# set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# -----------------------------------------------------------------------------
# 2. Find Dependencies
# -----------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED) # libcrypto: MD5 for offline-mode UUIDs, later AES/RSA for encryption
# GLM and ASIO are header-only; we just need to ensure the paths are known

# -----------------------------------------------------------------------------
# 3. Include Directories
# -----------------------------------------------------------------------------
include_directories(
    src
    src/network
    src/renderer
    src/renderer/terrain
    src/renderer/entity
    src/renderer/gui
    # Third party headers
    lib/vk-bootstrap/src
    lib/vulkanmemory/include
    lib/stb
    lib/imgui
    lib/imgui/backends
)

# -----------------------------------------------------------------------------
# 4. Source Gathering
# -----------------------------------------------------------------------------
# We use GLOB_RECURSE to find all .cpp files in src/ automatically
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Add vk-bootstrap.
list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/lib/vk-bootstrap/src/VkBootstrap.cpp")

# Add VMA.
list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/lib/vulkanmemory/include/vk_mem_alloc.h")

# Add ImGui core and backends
list(APPEND SOURCES 
    "${CMAKE_SOURCE_DIR}/lib/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/imgui_demo.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/backends/imgui_impl_glfw.cpp"
    "${CMAKE_SOURCE_DIR}/lib/imgui/backends/imgui_impl_vulkan.cpp"
)

# -----------------------------------------------------------------------------
# 5. Executable & Linking
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_options(${PROJECT_NAME} PRIVATE 
    -Wall 
    -Wextra
    -pthread
    -g                # Keep debugging symbols.
    -Og               # Debugger optimization
    -march=native     # Optimize for your specific OptiPlex CPU
    -ffast-math       # Faster floating point math (fine for games)
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    ZLIB::ZLIB
    OpenSSL::Crypto
    pthread
    dl
)

# -----------------------------------------------------------------------------
# 6. Resource Management
# -----------------------------------------------------------------------------
# Copy shaders and assets to the build directory so the app can find them
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${CMAKE_SOURCE_DIR}/resources 
    ${CMAKE_BINARY_DIR}/resources
)