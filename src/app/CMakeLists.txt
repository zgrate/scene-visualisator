qt_add_executable(SceneVisualiser WIN32 MACOSX_BUNDLE channel_grid.cpp channel_grid.h drawing_item.cpp drawing_item.h main.cpp session.cpp session.h universe_meter.cpp universe_meter.h
    viewport_item.cpp viewport_item.h)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(SceneVisualiser
    URI SceneVisualiser
    QML_FILES qml/Main.qml qml/Theme.qml qml/Card.qml qml/Field.qml qml/RailButton.qml qml/HudButton.qml
              qml/Objects.qml qml/Inspector.qml qml/ProjectScreen.qml
              qml/PatchScreen.qml qml/DmxScreen.qml qml/Input.qml qml/Check.qml
              qml/MediaCard.qml qml/MappingScreen.qml qml/PlanScreen.qml qml/ResolumeParams.qml
    RESOURCES qml/icons/design.svg qml/icons/patch.svg qml/icons/dmx.svg qml/icons/project.svg qml/icons/mapping.svg qml/icons/plan.svg)
file(GLOB sv_bundled_gdtf CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/resources/gdtf/*.gdtf)
qt_add_resources(SceneVisualiser "bundled_gdtf" PREFIX "/gdtf" BASE ${PROJECT_SOURCE_DIR}/resources/gdtf FILES ${sv_bundled_gdtf})
target_link_libraries(SceneVisualiser PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Svg sv_render sv_proto sv_media)
