# QRhi renderer. The only library allowed to use Qt (Gui + RHI), never Quick/QML.
add_library(sv_render STATIC
    camera.cpp camera.h
    drawing_painter.cpp drawing_painter.h
    scene_builder.cpp scene_builder.h
    transform_math.cpp transform_math.h
    scene_renderer.cpp scene_renderer.h
    test_pattern.cpp test_pattern.h)
target_include_directories(sv_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# QRhi headers (rhi/qrhi.h) ship with limited compatibility guarantees and need GuiPrivate.
target_link_libraries(sv_render PUBLIC Qt6::Gui Qt6::GuiPrivate sv_core sv_gdtf sv_proto)
# Vulkan-style GLSL -> .qsb (SPIR-V, GLSL, HLSL, MSL) at :/sv/shaders/<name>.qsb
qt_add_shaders(sv_render "sv_shaders"
    PREFIX "/sv"
    FILES
        shaders/grid.vert shaders/grid.frag
        shaders/mesh.vert shaders/mesh.frag
        shaders/wall.vert shaders/wall.frag
        shaders/beam.vert shaders/beam.frag)
