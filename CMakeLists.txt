project(beast_server)
cmake_minimum_required(VERSION 3.11)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()  

if (WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
endif()

file(GLOB headers src/*.h)
file(GLOB sources src/*.cpp)

add_executable(${PROJECT_NAME} ${headers} ${sources})
target_link_libraries(${PROJECT_NAME} ${CONAN_LIBS})
