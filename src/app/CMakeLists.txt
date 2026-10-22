qt_add_executable(SceneVisualiser WIN32 MACOSX_BUNDLE main.cpp session.cpp session.h viewport_item.cpp viewport_item.h)
qt_add_qml_module(SceneVisualiser
    URI SceneVisualiser
    QML_FILES qml/Main.qml)
file(GLOB sv_bundled_gdtf CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/resources/gdtf/*.gdtf)
qt_add_resources(SceneVisualiser "bundled_gdtf" PREFIX "/gdtf" BASE ${PROJECT_SOURCE_DIR}/resources/gdtf FILES ${sv_bundled_gdtf})
target_link_libraries(SceneVisualiser PRIVATE Qt6::Quick Qt6::QuickControls2 sv_render sv_proto)
