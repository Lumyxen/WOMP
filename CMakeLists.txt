cmake_minimum_required(VERSION 3.25)

project(womp
    VERSION 0.1.0
    DESCRIPTION "C++23 project"
    LANGUAGES CXX
)

add_executable(womp
    src/main.cpp
)

target_compile_features(womp PRIVATE cxx_std_23)

set_target_properties(womp PROPERTIES
    CXX_EXTENSIONS OFF
)
