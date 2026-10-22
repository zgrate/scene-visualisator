# Third-party code via FetchContent, pinned by tag (decision D4: no vcpkg in alpha).
# Only deps the code uses are declared: the OBJ reader (core/interchange) and the log file (app/main.cpp) are our own,
# so tinyobjloader and spdlog from the plan were not needed.
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

# GDTF description.xml parsing (D5).
FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/archive/refs/tags/v1.15.tar.gz
    URL_HASH SHA256=b39647064d9e28297a34278bfb897092bf33b7c487906ddfc094c9e8868bddcb
    SYSTEM)
set(PUGIXML_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

# Standalone Asio (header-only) for the network thread. sACN uses our own E1.31 receiver on it, not the ETCLabs
# library (the spec's named fallback): one socket model, no EtcPal threads.
FetchContent_Declare(asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8)
FetchContent_MakeAvailable(asio)
find_package(Threads REQUIRED)
add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(asio INTERFACE Threads::Threads)
if(WIN32)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
    target_link_libraries(asio INTERFACE ws2_32 mswsock)
endif()

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
