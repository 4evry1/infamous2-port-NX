# Included right after the port's project() call (CMAKE_PROJECT_INCLUDE).
# The engine's CMake only looks for glslang on WIN32 and UNIX hosts; a Switch build is neither, so the guest
# shader translator (HLSL -> SPIR-V) would compile as a stub and the Vulkan backend would stay on its
# fixed-function path. Finding the package here, in the top directory, makes glslang_FOUND and the glslang::
# targets visible to the engine's CMakeLists below it.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
find_package(glslang CONFIG REQUIRED)
message(STATUS "Switch: glslang ${glslang_VERSION} for guest shader programs (${glslang_DIR})")
