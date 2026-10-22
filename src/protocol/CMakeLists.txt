# Art-Net / sACN receivers, merger, frame handoff, internal sources. No Qt here.
add_library(sv_proto STATIC
    constants.h triple_buffer.h
    packets.cpp packets.h
    merger.cpp merger.h
    generators.cpp generators.h
    dmx_engine.cpp dmx_engine.h)
target_include_directories(sv_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sv_proto PRIVATE asio)
