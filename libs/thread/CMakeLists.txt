#接口库(只有头文件)
project(thread)
#链接此库目标会自动获得此路径下头文件
add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    GTest::gtest
    GTest::gtest_main
    pthread
)

add_test(NAME thread_pool_test COMMAND thread_pool_test.cpp)