# Live composition sources (FR-13, FR-14). Qt Gui + Multimedia, never Quick/QML.
add_library(sv_media STATIC media_source.cpp media_source.h)
target_include_directories(sv_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sv_media PUBLIC Qt6::Gui Qt6::Multimedia)
set_target_properties(sv_media PROPERTIES AUTOMOC ON)
