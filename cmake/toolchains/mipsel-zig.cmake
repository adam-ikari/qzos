# Cross toolchain: qzjs -> C1 Slim / MP-D261 (MIPS32r2 LE, hard-float, musl static)
# Compiler wrappers in .tools/bin bake in -target mipsel-linux-musleabihf.

get_filename_component(_qzos_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_qzos_tools "${_qzos_root}/.tools")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)

set(CMAKE_C_COMPILER "${_qzos_tools}/bin/zig-cc-mipsel")
set(CMAKE_CXX_COMPILER "${_qzos_tools}/bin/zig-cxx-mipsel")

# zig cc always embeds DWARF (even with -g0); strip at link instead.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--strip-all -Wl,--build-id=none")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--strip-all -Wl,--build-id=none")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
