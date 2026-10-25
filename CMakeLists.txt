# ES3 webOS probe: checks whether an LG webOS TV can run the Animal Crossing PC
# port's OpenGL ES 3 renderer (context, driver, the port's own shaders, and a
# full-screen GPU benchmark), and shows the result on the TV.
#
#   webOS:   cmake -B build-webos -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#            cmake --build build-webos --target webos-package
#   Desktop: cmake -B build && cmake --build build      (needs SDL2 and an ES 3 capable driver)

cmake_minimum_required(VERSION 3.21)
project(ES3WebOSProbe VERSION 0.1.0 LANGUAGES C CXX)

set(PROBE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${PROBE_ROOT}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The webosbrew toolchain's compiler reports "*-webos-*".
execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE PROBE_COMPILER_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(PROBE_COMPILER_TARGET MATCHES "webos")
    set(PROBE_TARGET_WEBOS ON)
else()
    set(PROBE_TARGET_WEBOS OFF)
endif()

set(PROBE_APP_ID "org.es3webosprobe.app" CACHE STRING "webOS application id")

# SDL2: the SDL-webOS backport on the TV (remote keys, Back delivery, panel
# resolution), the system SDL2 on a desktop.
if(PROBE_TARGET_WEBOS)
    set(SWM_SDL2_BACKPORT_RELEASE "release-2.30.12-webos.6" CACHE STRING "SDL-webOS release to bundle")
    include(SDL2WebOSBackport)
    set(PROBE_SDL2_RUNTIME_LIBRARY "${SWM_SDL2_RUNTIME_LIBRARY}")
else()
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
    endif()
endif()

# The port's shaders, embedded so the probe tests exactly what the game would compile.
set(PROBE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(stage IN ITEMS Vertex Fragment)
    if(stage STREQUAL "Vertex")
        set(source "${PROBE_ROOT}/third_party/acgc-shaders/default.vert")
    else()
        set(source "${PROBE_ROOT}/third_party/acgc-shaders/default.frag")
    endif()
    set(header "${PROBE_GENERATED_DIR}/Acgc${stage}Shader.generated.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${source} -DOUTPUT=${header} -DVARIABLE=kAcgc${stage}Shader
                -P "${PROBE_ROOT}/cmake/EmbedTextFile.cmake"
        DEPENDS "${source}" "${PROBE_ROOT}/cmake/EmbedTextFile.cmake"
        COMMENT "Embedding the port's ${stage} shader")
    list(APPEND PROBE_GENERATED_HEADERS "${header}")
endforeach()

add_executable(es3probe
    src/main.cpp
    src/FontData.cpp
    src/GlFunctions.cpp
    src/Probes.cpp
    src/Report.cpp
    src/TextRenderer.cpp
    ${PROBE_GENERATED_HEADERS}
)
target_compile_features(es3probe PRIVATE cxx_std_17)
set_target_properties(es3probe PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(es3probe PRIVATE
    "${PROBE_ROOT}/src"
    "${PROBE_GENERATED_DIR}"
    # Khronos ES 3 headers (types only); the webOS sysroot ships ES 2 headers only.
    "${PROBE_ROOT}/third_party/khronos")
target_compile_options(es3probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>)
# GL is reached through SDL_GL_GetProcAddress only, so no GL library is linked.
target_link_libraries(es3probe PRIVATE SDL2::SDL2)

if(PROBE_TARGET_WEBOS)
    # The TV's C++ runtime is older than the toolchain's; carry our own.
    target_link_options(es3probe PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(es3probe PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "\$ORIGIN/../lib")
    include(WebOSPackage)
endif()
