cmake_minimum_required(VERSION 3.15)
project(trademind LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
)

# Find dependencies
find_package(yaml-cpp REQUIRED)
find_package(Boost REQUIRED)
find_package(ZeroMQ REQUIRED)

# Find Fix8 (adjust these paths as needed)
find_path(FIX8_INCLUDE_DIR NAMES fix8/f8includes.hpp)
find_library(FIX8_LIBRARY NAMES Fix8)

if(NOT FIX8_INCLUDE_DIR)
    message(FATAL_ERROR "Fix8 include directory not found. Please set FIX8_INCLUDE_DIR.")
endif()
if(NOT FIX8_LIBRARY)
    message(FATAL_ERROR "Fix8 library not found. Please set FIX8_LIBRARY.")
endif()

include_directories(${FIX8_INCLUDE_DIR})

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Executable
add_executable(trademind ${SOURCES})

# Link libraries
target_link_libraries(trademind
    yaml-cpp
    Boost::boost
    ${ZeroMQ_LIBRARIES}
    ${FIX8_LIBRARY}
)

# Install rule
install(TARGETS trademind DESTINATION bin)