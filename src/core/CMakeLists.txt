# Project model, .svproj, undo, interchange. No Qt here (AGENTS.md "Layout & boundaries").
add_library(sv_core STATIC model.cpp model.h document.cpp document.h project_file.cpp project_file.h units.h)
target_include_directories(sv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sv_core PUBLIC nlohmann_json::nlohmann_json PRIVATE miniz)
