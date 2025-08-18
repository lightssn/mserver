#接口库(只有头文件)
project(thread)
#链接此库目标会自动获得此路径下头文件
add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})