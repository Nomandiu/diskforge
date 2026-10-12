cmake_minimum_required(VERSION 3.16)
project(DiskForgeEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(diskforge-engine STATIC
  src/engine/df/common.cpp
  src/engine/df/crc32.cpp
  src/engine/df/guid.cpp
  src/engine/df/units.cpp
  src/engine/df/diskio.cpp
  src/engine/df/mbr.cpp
  src/engine/df/gpt.cpp
  src/engine/df/layout.cpp
  src/engine/df/fsprobe.cpp
  src/engine/df/fat.cpp
  src/engine/df/ntfs.cpp
  src/engine/df/carve.cpp
  src/engine/df/scan.cpp
)

if(WIN32)
  target_sources(diskforge-engine PRIVATE
    src/engine/df/diskio_win.cpp
    src/engine/df/ops_win.cpp
  )
endif()

target_include_directories(diskforge-engine PUBLIC src/engine)

if(WIN32)
  target_link_libraries(diskforge-engine PUBLIC ole32)
endif()

add_executable(df-tests
  src/tests/tests_main.cpp
  src/tests/test_core.cpp
  src/tests/test_tables.cpp
  src/tests/test_fat.cpp
  src/tests/test_ntfs.cpp
  src/tests/test_carve.cpp
  src/tests/test_scan.cpp
)

if(WIN32)
  target_sources(df-tests PRIVATE src/tests/integration_win.cpp)
endif()

target_link_libraries(df-tests PRIVATE diskforge-engine)
target_include_directories(df-tests PRIVATE src/tests)

enable_testing()
add_test(NAME unit COMMAND df-tests)
