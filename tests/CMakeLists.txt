add_executable(sv_tests core_tests.cpp document_tests.cpp project_file_tests.cpp interchange_tests.cpp resolume_tests.cpp mvr_tests.cpp fbx_tests.cpp gdtf_tests.cpp protocol_tests.cpp render_tests.cpp mcp_tests.cpp)
target_link_libraries(sv_tests PRIVATE Catch2::Catch2WithMain sv_core sv_gdtf sv_proto sv_render sv_mcp asio pugixml::pugixml)
catch_discover_tests(sv_tests)
target_compile_definitions(sv_tests PRIVATE SV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(sv_render_golden render_golden.cpp)
target_link_libraries(sv_render_golden PRIVATE sv_render Qt6::Gui)
target_compile_definitions(sv_render_golden PRIVATE SV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# Pixel comparison only on Linux/llvmpipe, where the golden was made; other OSes just render and save the PNG.
find_program(XVFB_RUN xvfb-run)
if(UNIX AND NOT APPLE AND XVFB_RUN)
    add_test(NAME render_golden COMMAND ${XVFB_RUN} -a $<TARGET_FILE:sv_render_golden> ${PROJECT_SOURCE_DIR}/tests/golden/demo.png)
    set_tests_properties(render_golden PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
else()
    add_test(NAME render_golden COMMAND sv_render_golden ${PROJECT_SOURCE_DIR}/tests/golden/demo.png --no-compare)
endif()
set_property(TEST render_golden PROPERTY SKIP_RETURN_CODE 77)
# AI access end to end: the real app with --mcp, probed over HTTP (docs/mcp-plan.md C5).
if(UNIX AND NOT APPLE AND XVFB_RUN AND TARGET SceneVisualiser)
    add_test(NAME mcp_e2e COMMAND ${PROJECT_SOURCE_DIR}/tools/mcp_e2e.sh $<TARGET_FILE:SceneVisualiser>)
    set_tests_properties(mcp_e2e PROPERTIES TIMEOUT 90)
endif()

# Session (the QML-facing edit API) built straight from the app sources, under an offscreen QGuiApplication.
add_executable(sv_session_tests session_tests.cpp ${PROJECT_SOURCE_DIR}/src/app/session.cpp ${PROJECT_SOURCE_DIR}/src/app/session.h
    ${PROJECT_SOURCE_DIR}/src/app/updater.cpp ${PROJECT_SOURCE_DIR}/src/app/crash_reports.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ai_access.cpp)
set_target_properties(sv_session_tests PROPERTIES AUTOMOC ON)
target_include_directories(sv_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
file(GLOB sv_test_gdtf CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/resources/gdtf/*.gdtf)
qt_add_resources(sv_session_tests "test_gdtf" PREFIX "/gdtf" BASE ${PROJECT_SOURCE_DIR}/resources/gdtf FILES ${sv_test_gdtf})
target_link_libraries(sv_session_tests PRIVATE Catch2::Catch2 sv_render sv_proto sv_media sv_mcp Qt6::Quick Qt6::Network Qt6::Svg)
catch_discover_tests(sv_session_tests)
target_compile_definitions(sv_session_tests PRIVATE SV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# B7: the appcast signer used by release CI.
find_program(SV_PYTHON3 python3) # the one on PATH, as the tools are documented; find_package may pick another
if(SV_PYTHON3)
    add_test(NAME appcast_tool COMMAND ${SV_PYTHON3} -I ${PROJECT_SOURCE_DIR}/tools/make_appcast.py --self-test)
    set_tests_properties(appcast_tool PROPERTIES SKIP_RETURN_CODE 77)
endif()
