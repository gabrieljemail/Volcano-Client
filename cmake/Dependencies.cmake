# Third-party libraries that aren't vendored in third_party/ (ImGui, vk-bootstrap, VMA, nlohmann/json and stb are).
#
# Each one is taken, in order, from:
#   1. a prebuilt copy under VOLCANO_LIBS_DIR (the layout of the maintainer's C:/Apps/Libraries on Windows),
#   2. the system (find_package / find_path), which is what Linux and CI use,
#   3. a pinned download with FetchContent, where the library builds with CMake.
#
# Results, for the main CMakeLists.txt:
#   VOLCANO_DEP_INCLUDES   include directories (system)
#   VOLCANO_DEP_LIBS       libraries and imported targets to link
#   VOLCANO_RUNTIME_DLLS   DLLs to copy next to the exe (Windows, prebuilt copies only)

set(VOLCANO_DEP_INCLUDES "")
set(VOLCANO_DEP_LIBS "")
set(VOLCANO_RUNTIME_DLLS "")

set(_libs "${VOLCANO_LIBS_DIR}")
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- GLFW ---
if(WIN32 AND _libs AND EXISTS "${_libs}/GLFW/lib-vc2026/glfw3.lib")
    list(APPEND VOLCANO_DEP_INCLUDES "${_libs}/GLFW/include")
    list(APPEND VOLCANO_DEP_LIBS "${_libs}/GLFW/lib-vc2026/glfw3.lib")
else()
    find_package(glfw3 3.4 QUIET)
    if(glfw3_FOUND)
        list(APPEND VOLCANO_DEP_LIBS glfw)
    else()
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
        list(APPEND VOLCANO_DEP_LIBS glfw)
    endif()
endif()

# --- ZLIB ---
if(WIN32 AND _libs AND EXISTS "${_libs}/ZLIB/zlib.lib")
    list(APPEND VOLCANO_DEP_INCLUDES "${_libs}/ZLIB")
    list(APPEND VOLCANO_DEP_LIBS "${_libs}/ZLIB/zlib.lib")
else()
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        list(APPEND VOLCANO_DEP_LIBS ZLIB::ZLIB)
    else()
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.1 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(zlib)
        # zlib's own target doesn't export its include directories; zconf.h is generated into the binary dir.
        list(APPEND VOLCANO_DEP_INCLUDES "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
        list(APPEND VOLCANO_DEP_LIBS zlibstatic)
    endif()
endif()

# --- Asio (header only) ---
if(_libs AND EXISTS "${_libs}/Asio/include/asio.hpp")
    list(APPEND VOLCANO_DEP_INCLUDES "${_libs}/Asio/include")
else()
    find_path(ASIO_INCLUDE_DIR asio.hpp)
    if(ASIO_INCLUDE_DIR)
        list(APPEND VOLCANO_DEP_INCLUDES "${ASIO_INCLUDE_DIR}")
    else()
        FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git GIT_TAG asio-1-38-2 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(asio)
        list(APPEND VOLCANO_DEP_INCLUDES "${asio_SOURCE_DIR}/asio/include")
    endif()
endif()

# --- OpenAL ---
if(WIN32 AND _libs AND EXISTS "${_libs}/OpenAL/libs/Win64/OpenAL32.lib")
    list(APPEND VOLCANO_DEP_INCLUDES "${_libs}/OpenAL/include")
    list(APPEND VOLCANO_DEP_LIBS "${_libs}/OpenAL/libs/Win64/OpenAL32.lib")
else()
    find_package(OpenAL QUIET)
    if(OPENAL_FOUND)
        list(APPEND VOLCANO_DEP_LIBS OpenAL::OpenAL)
    else()
        set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
        set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
        set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
        set(LIBTYPE STATIC CACHE STRING "" FORCE)
        FetchContent_Declare(openal-soft GIT_REPOSITORY https://github.com/kcat/openal-soft.git GIT_TAG 1.24.3 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(openal-soft)
        list(APPEND VOLCANO_DEP_INCLUDES "${openal-soft_SOURCE_DIR}/include")
        list(APPEND VOLCANO_DEP_LIBS OpenAL)
    endif()
endif()

# --- glm (header only): the Vulkan SDK ships it, Linux has libglm-dev ---
find_path(GLM_INCLUDE_DIR glm/glm.hpp HINTS ${Vulkan_INCLUDE_DIRS} "${_libs}/GLM")
if(GLM_INCLUDE_DIR)
    list(APPEND VOLCANO_DEP_INCLUDES "${GLM_INCLUDE_DIR}")
else()
    FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glm)
    list(APPEND VOLCANO_DEP_INCLUDES "${glm_SOURCE_DIR}")
endif()

# --- OpenSSL ---
if(WIN32 AND _libs AND EXISTS "${_libs}/OpenSSL/lib/VC/x64/MD/libssl.lib")
    list(APPEND VOLCANO_DEP_INCLUDES "${_libs}/OpenSSL/include")
    list(APPEND VOLCANO_DEP_LIBS
        "${_libs}/OpenSSL/lib/VC/x64/MD/libcrypto.lib"
        "${_libs}/OpenSSL/lib/VC/x64/MD/libssl.lib")
    # The .lib files are import libraries; the loader needs the DLLs beside the exe.
    list(APPEND VOLCANO_RUNTIME_DLLS
        "${_libs}/OpenSSL/bin/libcrypto-4-x64.dll"
        "${_libs}/OpenSSL/bin/libssl-4-x64.dll")
else()
    # Never downloaded or built here: use the system's. Windows: install OpenSSL (for example `choco install openssl`,
    # or set OPENSSL_ROOT_DIR). Linux: libssl-dev.
    find_package(OpenSSL REQUIRED)
    list(APPEND VOLCANO_DEP_LIBS OpenSSL::SSL OpenSSL::Crypto)
endif()
