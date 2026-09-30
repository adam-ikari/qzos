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

# MIPS32 is 32-bit: there is no lock-free 8-byte atomic, so clang's
# -Watomic-alignment fires on every __atomic_*_8. That is structural, not a
# code defect — qzjs's mailbox/OOM counters are int64_t and QuickJS's BigInt
# atomics are 64-bit by language, so no source-level rewrite removes them. The
# compiler lowers them to compiler-rt/libatomic calls (correct, just slower).
# qzjs compiles its own sources under -Werror, so the warning has to be
# relaxed target-wide here; a per-file -Wno would be scattered guesswork.
set(CMAKE_C_FLAGS_INIT "-Wno-atomic-alignment")
set(CMAKE_CXX_FLAGS_INIT "-Wno-atomic-alignment")

# zig's bundled lld advertises --dependency-file in --help (the flag belongs to
# lld-link), but rejects it on ELF links. CMake >= 3.31 probes `ld --help` for
# that string and then believes it, so every executable link fails with
# "unsupported linker arg: --dependency-file=.../link.d". Opting out of the
# probe makes CMake fall back to the compiler-driven `-MD -MF link.d` depfile,
# which zig cc supports.
set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_DEPFILE_SUPPORTED FALSE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
