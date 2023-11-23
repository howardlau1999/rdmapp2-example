cmake_minimum_required(VERSION 3.3 FATAL_ERROR)
project(rdmapp-example)
set(CMAKE_CXX_STANDARD 20)

list(APPEND
  RDMAPP_EXAMPLE_COMPILE_OPTIONS
  PUBLIC
  -flto
  PRIVATE
  -fno-rtti
  -Wall 
  -Wextra 
  -pedantic 
  -Werror
)

if (CMAKE_CXX_COMPILER_ID MATCHES Clang)
  list(APPEND RDMAPP_EXAMPLE_COMPILE_OPTIONS
    PUBLIC
    -stdlib=libc++
    -fcoroutines-ts
  )
elseif (CMAKE_CXX_COMPILER_ID MATCHES GNU)
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "11") 
    list(APPEND RDMAPP_EXAMPLE_COMPILE_OPTIONS
      PUBLIC
      -fcoroutines
    )
  endif ()
endif ()

add_subdirectory(third_party/rdmapp)
set(LIBCORO_BUILD_TESTS OFF)
set(LIBCORO_FEATURE_SSL OFF)
set(LIBCORO_BUILD_EXAMPLES OFF)
add_subdirectory(third_party/libcoro)
include_directories(include)

add_executable(rdmapp-helloworld
  src/helloworld.cc
)
target_link_libraries(rdmapp-helloworld
  PUBLIC
  rdmapp
  libcoro
)
target_compile_options(rdmapp-helloworld ${RDMAPP_EXAMPLE_COMPILE_OPTIONS})