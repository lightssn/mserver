project(server)
add_executable(${PROJECT_NAME} "server.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE net)