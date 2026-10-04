# Cross-compiles for Windows x86-64 from Linux with MinGW-w64 GCC (Debian/Ubuntu package
# g++-mingw-w64-x86-64-posix), and runs the test executables under Wine (docs/development.md
# section 6.5). Libraries built for MinGW, such as OpenSSL from the MSYS2 mingw64 repository,
# go in a prefix named by the JARVIS_MINGW_PREFIX environment variable (holding include/ and lib/).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# The -posix variant: std::thread and std::mutex need the winpthreads threading model.
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
if(DEFINED ENV{JARVIS_MINGW_PREFIX})
  list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{JARVIS_MINGW_PREFIX}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The executables and plugins carry libstdc++, libgcc and winpthreads, so they run without the
# MinGW runtime DLLs on PATH; OpenSSL links statically for the same reason.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")
set(OPENSSL_USE_STATIC_LIBS TRUE)

# ctest runs the Windows executables through Wine; WINEPREFIX and WINEDEBUG come from the
# environment.
find_program(JARVIS_WINE NAMES wine64 wine PATHS /usr/lib/wine /usr/lib64/wine)
if(JARVIS_WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${JARVIS_WINE}")
endif()
