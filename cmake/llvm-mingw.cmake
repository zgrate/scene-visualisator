# Cross-compile for 64-bit Windows from Linux with llvm-mingw (clang, libc++, UCRT), the toolchain Qt's
# win64_llvm_mingw build uses. Put llvm-mingw's bin on PATH or set LLVM_MINGW to its root.
# Use with QT_ROOT_DIR=<Qt>/llvm-mingw_64 and QT_HOST_PATH=<Qt>/gcc_64 (host moc/rcc/qsb).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_bin "$ENV{LLVM_MINGW}/bin/")
if(NOT DEFINED ENV{LLVM_MINGW})
    set(_bin "")
endif()
set(CMAKE_C_COMPILER ${_bin}x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${_bin}x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${_bin}x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
