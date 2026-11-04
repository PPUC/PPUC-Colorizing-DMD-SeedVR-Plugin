cmake_minimum_required(VERSION 3.20)
project(ColorizingDMDSeedVR VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The editor API (ColorizingDMD's plugin-api, version 3) and the VPX plugin SDK it builds on
# (vpinball's plugins/plugins), as the editor is built with.
set(COLORIZINGDMD_DIR "" CACHE PATH "ColorizingDMD sources")
set(VPINBALL_DIR "" CACHE PATH "vpinball sources, for the plugin SDK")
# Where a development editor looks for plugins (its build's vpx-plugins folder): the plugin is
# copied there after each build when set.
set(COLORIZINGDMD_PLUGIN_DIR "" CACHE PATH "Editor plugin folder to copy the plugin into")

if(NOT EXISTS "${COLORIZINGDMD_DIR}/plugin-api/ColorizingDMDPlugin.h")
    message(FATAL_ERROR "Set COLORIZINGDMD_DIR to the ColorizingDMD sources.")
endif()
if(NOT EXISTS "${VPINBALL_DIR}/plugins/plugins/MsgPlugin.h")
    message(FATAL_ERROR "Set VPINBALL_DIR to the vpinball sources (plugins/plugins).")
endif()

find_package(Threads REQUIRED)

# Everything but the entry points, for the plugin and the tests.
add_library(seedvr_core STATIC
    src/Pictures.cpp
    src/Pictures.h
    src/ServiceClient.cpp
    src/ServiceClient.h
    src/Upscale.cpp
    src/Upscale.h
)
target_include_directories(seedvr_core PUBLIC src)
target_include_directories(seedvr_core SYSTEM PUBLIC
    "${COLORIZINGDMD_DIR}/plugin-api"
    "${VPINBALL_DIR}/plugins"
    "${PROJECT_SOURCE_DIR}/third-party"
    "${PROJECT_SOURCE_DIR}/third-party/httplib"
)
target_link_libraries(seedvr_core PUBLIC Threads::Threads)
set_target_properties(seedvr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(seedvr_core PUBLIC ws2_32)
    # Windows 10 and later (httplib needs Vista or later).
    target_compile_definitions(seedvr_core PUBLIC _WIN32_WINNT=0x0A00)
endif()

add_library(SeedVRPlugin MODULE src/Plugin.cpp)
target_link_libraries(SeedVRPlugin PRIVATE seedvr_core)

foreach(target seedvr_core SeedVRPlugin)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# The file names plugin.cfg gives.
set_target_properties(SeedVRPlugin PROPERTIES PREFIX "" OUTPUT_NAME plugin-seedvr)
if(WIN32)
    set_target_properties(SeedVRPlugin PROPERTIES OUTPUT_NAME plugin-seedvr64)
elseif(APPLE)
    set_target_properties(SeedVRPlugin PROPERTIES SUFFIX ".dylib")
endif()

# The plugin's folder, as the editor's plugin folder holds it: SeedVR/plugin.cfg and the
# library.
set(plugin_folder "${CMAKE_BINARY_DIR}/SeedVR")
add_custom_command(TARGET SeedVRPlugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${plugin_folder}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/plugin.cfg" "${plugin_folder}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:SeedVRPlugin>" "${plugin_folder}/"
    VERBATIM
)
if(COLORIZINGDMD_PLUGIN_DIR)
    add_custom_command(TARGET SeedVRPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${plugin_folder}"
                "${COLORIZINGDMD_PLUGIN_DIR}/SeedVR"
        VERBATIM
    )
endif()

# Tests: the picture logic, the client against a stub service in the test, and the run on a
# fake editor; the Python service with its stub backend when Python 3 is found.
include(CTest)
if(BUILD_TESTING)
    add_executable(seedvr_tests tests/SeedVRTests.cpp)
    target_link_libraries(seedvr_tests PRIVATE seedvr_core)
    if(NOT MSVC)
        target_compile_options(seedvr_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME seedvr_tests COMMAND seedvr_tests)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        # The service's own tests, and the plugin's client against the service (stub backend),
        # as the plugin talks to it: the test program run with --service.
        add_test(NAME seedvr_service_tests
                 COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                         "${PROJECT_SOURCE_DIR}/service" -p "test_*.py" -v)
        set_tests_properties(seedvr_service_tests PROPERTIES
            ENVIRONMENT "SEEDVR_TESTS_BINARY=$<TARGET_FILE:seedvr_tests>")
    endif()
endif()
