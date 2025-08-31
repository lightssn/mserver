cmake_minimum_required(VERSION 3.22)

if(WIN32)
    set(CMAKE_TOOLCHAIN_FILE "F:/source/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg路径")
    set(GTest_DIR "F:/source/vcpkg/packages/gtest_x64-windows/share/gtest" CACHE STRING "gtest路径" FORCE)
endif()

#project(YourProjectName VERSION 1.0.0 LANGUAGES CXX

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")#Release优化选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build Type" FORCE)#默认使用Release构建
endif()

option(THREAD_BUILD_TESTS "Build thread library tests" ON)

enable_testing()
find_package(GTest REQUIRED)
#F:/source/vcpkg/packages/gtest_x64-windows/share/gtest

add_subdirectory(libs/thread)
add_subdirectory(libs/net)
add_subdirectory(apps/client)
add_subdirectory(apps/server)