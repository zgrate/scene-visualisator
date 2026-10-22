add_executable(sv_tests core_tests.cpp document_tests.cpp project_file_tests.cpp gdtf_tests.cpp protocol_tests.cpp render_tests.cpp)
target_link_libraries(sv_tests PRIVATE Catch2::Catch2WithMain sv_core sv_gdtf sv_proto sv_render asio)
catch_discover_tests(sv_tests)
target_compile_definitions(sv_tests PRIVATE SV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
