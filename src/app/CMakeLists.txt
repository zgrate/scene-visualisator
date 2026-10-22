qt_add_executable(SceneVisualiser WIN32 MACOSX_BUNDLE channel_grid.cpp channel_grid.h drawing_item.cpp drawing_item.h ai_access.cpp ai_access.h main.cpp session.cpp session.h universe_meter.cpp universe_meter.h
    viewport_item.cpp viewport_item.h updater.cpp updater.h crash_reports.cpp crash_reports.h)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(SceneVisualiser
    URI SceneVisualiser
    QML_FILES qml/Main.qml qml/Theme.qml qml/Card.qml qml/Field.qml qml/RailButton.qml qml/HudButton.qml
              qml/Objects.qml qml/Inspector.qml qml/ProjectScreen.qml
              qml/PatchScreen.qml qml/DmxScreen.qml qml/Input.qml qml/Check.qml
              qml/MediaCard.qml qml/MappingScreen.qml qml/PlanScreen.qml qml/ResolumeParams.qml qml/AiPanel.qml
    RESOURCES qml/icons/design.svg qml/icons/patch.svg qml/icons/dmx.svg qml/icons/project.svg qml/icons/mapping.svg qml/icons/plan.svg)
file(GLOB sv_bundled_gdtf CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/resources/gdtf/*.gdtf)
qt_add_resources(SceneVisualiser "bundled_gdtf" PREFIX "/gdtf" BASE ${PROJECT_SOURCE_DIR}/resources/gdtf FILES ${sv_bundled_gdtf})
target_link_libraries(SceneVisualiser PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Svg sv_render sv_proto sv_media sv_mcp Qt6::Network)
target_compile_definitions(SceneVisualiser PRIVATE SV_VERSION="${PROJECT_VERSION}")

# B7 release integration, all optional: local builds get no updater and no Sentry. Release CI sets these.
set(SV_APPCAST_BASE "" CACHE STRING "Update feed folder URL (appcast.xml, appcast-test.xml); empty: no updater")
set(SV_UPDATE_PUBLIC_KEY "" CACHE STRING "Base64 EdDSA public key the appcast and downloads are signed with")
set(SV_WINSPARKLE_DIR "" CACHE PATH "Unpacked WinSparkle release (include/, x64/Release/)")
set(SV_SPARKLE_DIR "" CACHE PATH "Unpacked Sparkle 2 release (Sparkle.framework)")
if(SV_APPCAST_BASE AND ((WIN32 AND SV_WINSPARKLE_DIR) OR (APPLE AND SV_SPARKLE_DIR)))
    target_compile_definitions(SceneVisualiser PRIVATE SV_APPCAST_BASE="${SV_APPCAST_BASE}")
    if(SV_UPDATE_PUBLIC_KEY)
        target_compile_definitions(SceneVisualiser PRIVATE SV_UPDATE_PUBLIC_KEY="${SV_UPDATE_PUBLIC_KEY}")
    endif()
    if(WIN32)
        target_include_directories(SceneVisualiser PRIVATE ${SV_WINSPARKLE_DIR}/include)
        target_link_libraries(SceneVisualiser PRIVATE ${SV_WINSPARKLE_DIR}/x64/Release/WinSparkle.lib)
        add_custom_command(TARGET SceneVisualiser POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SV_WINSPARKLE_DIR}/x64/Release/WinSparkle.dll $<TARGET_FILE_DIR:SceneVisualiser>)
    else()
        target_sources(SceneVisualiser PRIVATE updater_mac.mm)
        set_source_files_properties(updater_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
        target_compile_options(SceneVisualiser PRIVATE -F${SV_SPARKLE_DIR})
        target_link_options(SceneVisualiser PRIVATE -F${SV_SPARKLE_DIR})
        target_link_libraries(SceneVisualiser PRIVATE "-framework Sparkle")
        set_target_properties(SceneVisualiser PROPERTIES BUILD_RPATH "@executable_path/../Frameworks"
                                                         INSTALL_RPATH "@executable_path/../Frameworks")
        add_custom_command(TARGET SceneVisualiser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:SceneVisualiser>/Frameworks
            COMMAND ditto ${SV_SPARKLE_DIR}/Sparkle.framework
                    $<TARGET_BUNDLE_CONTENT_DIR:SceneVisualiser>/Frameworks/Sparkle.framework)
    endif()
endif()

set(SV_SENTRY_DSN "" CACHE STRING "Sentry DSN for opt-in crash uploads; empty: Sentry is compiled out")
if(SV_SENTRY_DSN)
    target_compile_definitions(SceneVisualiser PRIVATE SV_SENTRY_DSN="${SV_SENTRY_DSN}")
    target_link_libraries(SceneVisualiser PRIVATE sentry::sentry)
elseif(WIN32)
    target_link_libraries(SceneVisualiser PRIVATE dbghelp) # the plain minidump handler
endif()

if(APPLE)
    # Local-network prompt text, Bonjour for NDI, .svproj documents, and the Sparkle feed/key (B7).
    set_target_properties(SceneVisualiser PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/installer/macos/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER org.scenevisualiser.app
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
endif()
