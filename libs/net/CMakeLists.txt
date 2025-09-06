#构建静态库并指定头文件目录
project(net)
#添加子项目，EXCLUDE_FROM_ALL排除在默认构建之外，执行make时不会构建fmt目录
add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(json)

#递归查找
file(GLOB_RECURSE SOURCE_FILE "src/*.cpp")
#创建静态库
add_library(${PROJECT_NAME} STATIC ${SOURCE_FILE})
#链接子项目库
target_link_libraries(${PROJECT_NAME} PUBLIC fmt::fmt)
target_link_libraries(${PROJECT_NAME} PUBLIC json_lib)
target_link_libraries(${PROJECT_NAME} PRIVATE thread)
#PUBLIC使依赖此库的目标自动获得头文件路径
target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE ${Qt5Core_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Xml Qt5::Sql Qt5::Concurrent)