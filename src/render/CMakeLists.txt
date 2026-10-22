# QRhi renderer. The only library allowed to use Qt (Gui + RHI), never Quick/QML.
add_library(sv_render STATIC scene_renderer.cpp scene_renderer.h)
target_include_directories(sv_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# QRhi headers (rhi/qrhi.h) ship with limited compatibility guarantees and need GuiPrivate.
target_link_libraries(sv_render PUBLIC Qt6::Gui Qt6::GuiPrivate sv_core sv_gdtf)
