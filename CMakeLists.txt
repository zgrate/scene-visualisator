cmake_minimum_required(VERSION 3.28)
project(SceneVisualiser VERSION 0.3.0 LANGUAGES CXX)

# Executables land in build/<preset>/bin; the QML module folder of the same name stays in src/app.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
if(APPLE)
    enable_language(C OBJC OBJCXX) # Syphon
endif()
include(cmake/deps.cmake)

find_package(Qt6 6.12 REQUIRED COMPONENTS Gui GuiPrivate Quick QuickControls2 ShaderTools Multimedia Svg)
qt_standard_project_setup(REQUIRES 6.12)
# Our structs use `slots` (DMX slots, wheel slots); use Q_SIGNALS/Q_SLOTS/Q_EMIT in Qt code.
add_compile_definitions(QT_NO_KEYWORDS)

add_subdirectory(src/core)
add_subdirectory(src/gdtf)
add_subdirectory(src/protocol)
add_subdirectory(src/render)
add_subdirectory(src/media)
add_subdirectory(src/app)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
