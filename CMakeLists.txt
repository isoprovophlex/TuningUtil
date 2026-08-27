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

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/LumaUtil/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/MMSF/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/Heliosphan/include
)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(DirectXTK CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
    CommonLibSSE::CommonLibSSE
    reflectcpp::reflectcpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

if(MSVC)
  target_compile_options(
    ${PROJECT_NAME}
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
    ${PROJECT_NAME}
    PRIVATE
      "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
      "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
  )
endif()

option(TUNINGUTIL_BUILD_TESTS "Build TuningUtil tests" ON)

if(TUNINGUTIL_BUILD_TESTS)
  enable_testing()

  add_executable(
    JsonOverlayTests
    tests/JsonOverlayTests.cpp
    src/JsonOverlay.cpp
    src/PresetCatalog.cpp
    src/UserSettings.cpp
  )
  target_compile_features(JsonOverlayTests PRIVATE cxx_std_23)
  target_include_directories(JsonOverlayTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(JsonOverlayTests PRIVATE reflectcpp::reflectcpp)

  add_executable(
    SliderCreatorTests
    tests/SliderCreatorTests.cpp
    src/PresetCatalog.cpp
    src/SliderCreator.cpp
    src/SliderSettingCatalog.cpp
  )
  target_compile_features(SliderCreatorTests PRIVATE cxx_std_23)
  target_include_directories(SliderCreatorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(SliderCreatorTests PRIVATE reflectcpp::reflectcpp)

  set(
    TUNINGUTIL_ADDRESS_LIBRARY_1_7_99
    ""
    CACHE FILEPATH
    "Path to the exact Skyrim 1.7.99 Address Library binary"
  )

  if(NOT TUNINGUTIL_ADDRESS_LIBRARY_1_7_99 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    find_file(
      TUNINGUTIL_PACKAGED_ADDRESS_LIBRARY_1_7_99
      NAMES versionlib-1-7-99-0.bin
      PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/commonlibsse-ng/test-data"
      NO_DEFAULT_PATH
    )
    if(TUNINGUTIL_PACKAGED_ADDRESS_LIBRARY_1_7_99)
      set(
        TUNINGUTIL_ADDRESS_LIBRARY_1_7_99
        "${TUNINGUTIL_PACKAGED_ADDRESS_LIBRARY_1_7_99}"
        CACHE FILEPATH
        "Path to the exact Skyrim 1.7.99 Address Library binary"
        FORCE
      )
    endif()
  endif()

  if(TUNINGUTIL_ADDRESS_LIBRARY_1_7_99)
    add_executable(
      TuningUtilAddressLibraryTests
      tests/AddressLibrary1799Tests.cpp
    )
    target_compile_features(TuningUtilAddressLibraryTests PRIVATE cxx_std_23)
    target_include_directories(TuningUtilAddressLibraryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(TuningUtilAddressLibraryTests PRIVATE ENABLE_COMMONLIBSSE_TESTING=1)
    target_link_libraries(TuningUtilAddressLibraryTests PRIVATE CommonLibSSE::CommonLibSSE)
    target_precompile_headers(TuningUtilAddressLibraryTests PRIVATE include/PCH.h)
  else()
    message(WARNING "The exact Skyrim 1.7.99 Address Library loader test is unavailable.")
  endif()

  if(MSVC)
    foreach(test_target IN ITEMS JsonOverlayTests SliderCreatorTests)
      target_compile_options(
        ${test_target}
        PRIVATE
          /utf-8
          /permissive-
          /Zc:preprocessor
      )
    endforeach()
    if(TARGET TuningUtilAddressLibraryTests)
      target_compile_options(
        TuningUtilAddressLibraryTests
        PRIVATE
          /utf-8
          /permissive-
          /Zc:preprocessor
      )
    endif()
  endif()

  add_test(NAME JsonOverlayTests COMMAND JsonOverlayTests)
  add_test(NAME SliderCreatorTests COMMAND SliderCreatorTests)
  if(TARGET TuningUtilAddressLibraryTests)
    add_test(
      NAME TuningUtilAddressLibrary1799
      COMMAND TuningUtilAddressLibraryTests "${TUNINGUTIL_ADDRESS_LIBRARY_1_7_99}"
    )
  endif()
endif()
