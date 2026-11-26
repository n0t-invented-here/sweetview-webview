set(CMAKE_SYSTEM_NAME Darwin)

set(CMAKE_C_COMPILER "clang${WEBVIEW_TOOLCHAIN_EXECUTABLE_SUFFIX}")
set(CMAKE_CXX_COMPILER "clang++${WEBVIEW_TOOLCHAIN_EXECUTABLE_SUFFIX}")

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE INTERNAL "")

# A universal build compiles every object as a fat file. With an LLVM
# distribution first on PATH, CMake pairs Apple's ar with llvm-ranlib, which
# does not take a fat object for Mach-O and rewrites the archive in GNU
# format; Apple's ld then refuses it ("archive member '//' not a mach-o
# file") and nothing that links a static library (the tests, the static
# webview library) builds. Apple's own archiver and ranlib handle fat objects.
set(CMAKE_AR "/usr/bin/ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "/usr/bin/ranlib" CACHE FILEPATH "")
