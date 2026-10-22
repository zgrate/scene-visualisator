cmake_minimum_required(VERSION 3.28)
project(SceneVisualiser VERSION 0.1.0 LANGUAGES CXX)

# Executables land in build/<preset>/bin; the QML module folder of the same name stays in src/app.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(cmake/deps.cmake)

find_package(Qt6 6.12 REQUIRED COMPONENTS Gui GuiPrivate Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.12)

add_subdirectory(src/core)
add_subdirectory(src/gdtf)
add_subdirectory(src/protocol)
add_subdirectory(src/render)
add_subdirectory(src/app)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
