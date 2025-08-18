cmake_minimum_required(VERSION 3.22)

#project(YourProjectName VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")#Release优化选项
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build Type" FORCE)#默认使用Release构建
endif()

add_subdirectory(libs/thread)
add_subdirectory(libs/net)
add_subdirectory(apps/client)
add_subdirectory(apps/server)

