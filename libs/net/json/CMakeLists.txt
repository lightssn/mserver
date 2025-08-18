

add_library(json_lib STATIC Json.cpp JsonParser.cpp)
target_include_directories(json_lib PUBLIC ./include)
