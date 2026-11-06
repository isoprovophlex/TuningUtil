cmake_minimum_required(VERSION 3.40)

set(NAME "TuningUtil" CACHE STRING "")
set(VERSION 0.1.0.0 CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)

if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
else()
  message(WARNING "Variable VCPKG_ROOT is not set. Continuing without vcpkg.")
endif()

project(
  ${NAME}
  VERSION ${VERSION}
  LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed.")
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

include(CMake/headerlist.cmake)
include(CMake/sourcelist.cmake)
list(FILTER sources EXCLUDE REGEX "/Headless(Main|Profile)\\.cpp$")

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${headers} ${sources}
)

add_library(
  ${PROJECT_NAME}
  SHARED
  ${headers}
  ${sources}
)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(DirectXTK CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)

set(headless_common_sources
  src/HeadlessMain.cpp
  src/HeadlessProfile.cpp
  src/ProfileParsing.cpp
  src/ProfileSettings.cpp
  src/JsonOverlay.cpp
  src/SliderStorage.cpp
  src/SliderSettingCatalog.cpp
  src/SettingLinks.cpp
  src/ImageSpacePatcher.cpp
  src/DetailedLogging.cpp
)

add_library(ATLMTuning SHARED EXCLUDE_FROM_ALL
  ${headless_common_sources}
  src/LightingPatcher.cpp
  src/RecordFilter.cpp
  src/CSTonemapping.cpp
  src/HeliosphanClient.cpp
)
add_library(HeliosTuning SHARED EXCLUDE_FROM_ALL
  ${headless_common_sources}
  src/WeatherPatcher.cpp
  src/WeatherRuntime.cpp
)
target_compile_definitions(ATLMTuning PRIVATE TUNING_HEADLESS TUNING_ATLM TUNING_PLUGIN_NAME="ATLMTuning")
target_compile_definitions(HeliosTuning PRIVATE TUNING_HEADLESS TUNING_HELIOS TUNING_PLUGIN_NAME="HeliosTuning")

foreach(target IN ITEMS ${PROJECT_NAME} ATLMTuning HeliosTuning)
  target_compile_features(${target} PRIVATE cxx_std_23)

  target_include_directories(
    ${target}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/extern/LumaUtil/include
      ${CMAKE_CURRENT_SOURCE_DIR}/extern/MMSF/include
      ${CMAKE_CURRENT_SOURCE_DIR}/extern/Heliosphan/include
  )

  target_link_libraries(
    ${target}
    PRIVATE
      CommonLibSSE::CommonLibSSE
      reflectcpp::reflectcpp
  )

  target_precompile_headers(${target} PRIVATE include/PCH.h)

  if(MSVC)
    target_compile_options(
      ${target}
      PRIVATE
        /sdl
        /utf-8
        /Zi
        /MD
        /permissive-
        /Zc:preprocessor
        /wd4099
        /wd4100
        /wd4200
        /wd4456
        /wd4505
        /wd4702
        /wd5054
        "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
    )

    target_link_options(
      ${target}
      PRIVATE
        "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
        "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
    )
  endif()

endforeach()
