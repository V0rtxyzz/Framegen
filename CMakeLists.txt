cmake_minimum_required(VERSION 3.22)
project(framegen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FRAMEGEN_BUILD_TESTS "Build host unit tests" OFF)

set(FRAMEGEN_CORE_SOURCES
    src/FrameGen.cpp
    src/EglHooks.cpp
)

if(ANDROID)
    include(FetchContent)

    # Override with -DFRAMEGEN_PRELOADER_REF=<tag|branch|commit> to pin a specific SDK version.
    # "main" matches current LeviLaunchroid builds (tag 0.2.2 has an older ModMenu ABI).
    set(FRAMEGEN_PRELOADER_REF "main" CACHE STRING "preloader-android git ref")

    FetchContent_Declare(
        preloader
        GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
        GIT_TAG ${FRAMEGEN_PRELOADER_REF}
    )
    FetchContent_MakeAvailable(preloader)

    # The link target name differs between preloader-android versions, so detect it.
    set(FRAMEGEN_PRELOADER_TARGET "")
    foreach(_candidate preloader::preloader preloader preloader_android preloader-android)
        if(TARGET ${_candidate})
            set(FRAMEGEN_PRELOADER_TARGET ${_candidate})
            break()
        endif()
    endforeach()
    message(STATUS "FrameGen links against preloader target: ${FRAMEGEN_PRELOADER_TARGET}")

    add_library(framegen SHARED
        ${FRAMEGEN_CORE_SOURCES}
        src/FrameGenMod.cpp
    )

    target_include_directories(framegen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${preloader_SOURCE_DIR}/include
    )

    target_compile_options(framegen PRIVATE
        -O3
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
    )

    find_library(LOG_LIB log)
    find_library(EGL_LIB EGL)
    find_library(GLESV3_LIB GLESv3)

    if(FRAMEGEN_PRELOADER_TARGET)
        target_link_libraries(framegen PRIVATE ${FRAMEGEN_PRELOADER_TARGET})
    else()
        message(WARNING "No preloader link target found; building with headers only.")
        target_link_options(framegen PRIVATE "-Wl,-z,undefs")
    endif()

    # Header dependencies pulled in by preloader-android (nlohmann/json, fmt)
    foreach(_dep nlohmann_json::nlohmann_json fmt::fmt)
        if(TARGET ${_dep})
            target_link_libraries(framegen PRIVATE ${_dep})
        endif()
    endforeach()

    target_link_libraries(framegen PRIVATE ${LOG_LIB} ${EGL_LIB} ${GLESV3_LIB})

    target_link_options(framegen PRIVATE
        "-Wl,--gc-sections"
        "-Wl,-s"
        "-Wl,-z,max-page-size=16384"
    )
endif()

if(FRAMEGEN_BUILD_TESTS AND NOT ANDROID)
    add_executable(framegen_tests tests/test_framegen.cpp ${FRAMEGEN_CORE_SOURCES})
    target_include_directories(framegen_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
