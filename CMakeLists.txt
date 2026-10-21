cmake_minimum_required(VERSION 3.21)
project(amd_hypervisor)

# Windows Kernel Driver build — requires WDK and Visual Studio
# Build with: cmake -G "Visual Studio 17 2022" -A x64 .
# Then: msbuild amd_hypervisor.sln /p:Configuration=Release /p:Platform=x64

# We don't build via cmake directly for WDK; this is for IDE indexing only.
# Use the Sources/Makefile (WDK 7) or the .vcxproj (WDK 10+) below.

set(SOURCES
    src/entry.cpp
    src/svm.cpp
    src/npt.cpp
    src/exit_handler.cpp
    src/asm/svm_asm.asm
)

set(HEADERS
    include/vmcb.h
    include/common.h
    include/svm.h
    include/npt.h
)

# Dummy target for IDE
add_library(hv_index STATIC ${SOURCES} ${HEADERS})
target_include_directories(hv_index PRIVATE include)
