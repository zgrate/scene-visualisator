# Third-party code via FetchContent, pinned by tag (decision D4: no vcpkg in alpha).
# shortcut: only deps the current milestone uses are declared, add pugixml/asio/spdlog/tinyobjloader/
# ETCLabs sACN here in the milestone that first includes them (M2-M6).
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM)
# Zip for .svproj and .gdtf packages. miniz instead of minizip-ng: one small MIT library, no zlib dependency.
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz
    URL_HASH SHA256=c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018
    SYSTEM)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # miniz 3.0.2 asks for CMake < 3.5, which CMake 4 rejects
FetchContent_MakeAvailable(nlohmann_json miniz)
unset(CMAKE_POLICY_VERSION_MINIMUM)

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
