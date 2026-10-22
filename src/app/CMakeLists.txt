qt_add_executable(SceneVisualiser WIN32 MACOSX_BUNDLE main.cpp viewport_item.cpp viewport_item.h)
qt_add_qml_module(SceneVisualiser
    URI SceneVisualiser
    QML_FILES qml/Main.qml)
target_link_libraries(SceneVisualiser PRIVATE Qt6::Quick Qt6::QuickControls2 sv_render sv_proto)
