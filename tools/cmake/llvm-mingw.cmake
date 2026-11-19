set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(MTF_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The installer names its own tree; an environment variable, because CMake reads this file again in each try_compile.
if(DEFINED ENV{MTF_LLVM_MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{MTF_LLVM_MINGW_ROOT}" MTF_LLVM_MINGW_ROOT)
else()
    set(MTF_LLVM_MINGW_ROOT "${MTF_PROJECT_ROOT}/build/toolchain/llvm-mingw-20261104-ucrt-x86_64")
endif()

set(CMAKE_C_COMPILER "${MTF_LLVM_MINGW_ROOT}/bin/clang.exe")
set(CMAKE_C_COMPILER_TARGET "x86_64-w64-windows-gnu")
set(CMAKE_AR "${MTF_LLVM_MINGW_ROOT}/bin/llvm-ar.exe")
set(CMAKE_RANLIB "${MTF_LLVM_MINGW_ROOT}/bin/llvm-ranlib.exe")
set(CMAKE_RC_COMPILER "${MTF_LLVM_MINGW_ROOT}/bin/llvm-windres.exe")
