# Ubuntu 24.04's newest clang on libstdc++ (clang 18 lacks the concepts level libstdc++ 13 needs for std::expected);
# only the optimisation flags are ours.
set(CMAKE_C_COMPILER clang-20)
set(CMAKE_CXX_COMPILER clang++-20)

set(CMAKE_C_FLAGS_RELEASE_INIT "-O3 -march=native -ffast-math -flto=thin -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -march=native -ffast-math -flto=thin -DNDEBUG")

# lld: ld.bfd cannot link clang's LTO objects without the LLVMgold plugin.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld-20")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
