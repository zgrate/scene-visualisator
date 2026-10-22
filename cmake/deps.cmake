# Third-party code via FetchContent, pinned by tag (decision D4: no vcpkg in alpha).
# shortcut: only deps the current milestone uses are declared, add nlohmann_json/pugixml/minizip-ng/asio/
# spdlog/tinyobjloader/ETCLabs sACN here in the milestone that first includes them (M1-M6).
include(FetchContent)

if(BUILD_TESTING)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
endif()
