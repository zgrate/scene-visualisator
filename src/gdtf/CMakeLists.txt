# GDTF reader and DMX-to-attribute decoder (decision D5). No Qt here.
add_library(sv_gdtf STATIC
    dmx_value.h
    fixture_type.cpp fixture_type.h
    decoder.cpp decoder.h)
target_include_directories(sv_gdtf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sv_gdtf PRIVATE pugixml::pugixml miniz)
