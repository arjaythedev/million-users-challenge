set(CMAKE_C_FLAGS_RELEASE_INIT "-O3 -march=native -ffast-math -flto=auto -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -march=native -ffast-math -flto=auto -DNDEBUG")

set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
