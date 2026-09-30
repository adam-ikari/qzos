# qzos — C1 Slim / MP-D261 e-ink OS host
#
# 三层结构（brain: qzos-ui-architecture / qzos-services-rpc）：
#   qzjs-rt 进程（ISOLATED JS 引擎） ←JSON→ qzos-host（本目标：LVGL + epaper +
#   evdev + uvrpc 系统服务，共享一个 libuv loop）。
#
# 本文件是顶层：
#   cmake -S os -B build-os [-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mipsel-zig.cmake]
cmake_minimum_required(VERSION 3.16)
project(qzos LANGUAGES C CXX)

get_filename_component(QZOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# qzjs（内含 uv_a：唯一 libuv 实例，uvrpc/LVGL/宿主全部复用它）
set(QZ_PROFILE "minimal" CACHE STRING "qzjs build profile")
add_subdirectory(${QZOS_ROOT}/qzjs ${CMAKE_CURRENT_BINARY_DIR}/qzjs)

# LVGL：lv_conf.h 固定在仓库根；关 demos/examples/thorvg（e-ink 宿主用不到）
set(LV_BUILD_CONF_PATH "${QZOS_ROOT}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
add_subdirectory(${QZOS_ROOT}/third_party/lvgl ${CMAKE_CURRENT_BINARY_DIR}/lvgl)

# ----------------------------------------------------------------------------
# flatcc runtime（FlatBuffers C 运行时；不依赖 uvrpc 顶层 CMake/mimalloc）
#
# 上游归属：uvrpc 是 submodule（保持上游干净），但它依赖的 flatcc runtime
# 与 uthash 由 qzos 自己 vendor 到 os/third_party/ —— 因为 uvrpc 顶层 CMake
# 的 deps 查找链在本项目不可用（mimalloc 默认、强制输出目录）。
# flatcc 生成头同理由 scripts/gen-uvrpc-schema.sh 在宿主机生成后提交。
# 见 brain qzos-services-rpc。
# ----------------------------------------------------------------------------
set(UV_DIR ${QZOS_ROOT}/third_party/uvrpc)
set(QZ_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
add_library(qzos_flatcc STATIC
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/builder.c
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/emitter.c
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/refmap.c
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/verifier.c
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/json_parser.c
    ${QZ_VENDOR_DIR}/flatcc/src/runtime/json_printer.c
)
target_include_directories(qzos_flatcc PUBLIC ${QZ_VENDOR_DIR}/flatcc/include)
target_compile_definitions(qzos_flatcc PRIVATE _POSIX_C_SOURCE=200809L)

# ----------------------------------------------------------------------------
# uvrpc（system allocator；源清单 = 上游顶层 CMakeLists 的 UVRPC_SOURCES；
# 禁止使用上游 deps/libuv —— libuv 单实例约束见 brain qzos-services-rpc）
# ----------------------------------------------------------------------------
add_library(qzos_uvrpc STATIC
    ${UV_DIR}/src/uvrpc_context.c
    ${UV_DIR}/src/uvrpc_flatbuffers.c
    ${UV_DIR}/src/uvrpc_server.c
    ${UV_DIR}/src/uvrpc_client.c
    ${UV_DIR}/src/uvrpc_service.c
    ${UV_DIR}/src/uvrpc_msgid.c
    ${UV_DIR}/src/uvrpc_idmap.c
    ${UV_DIR}/src/uvrpc_allocator.c
    ${UV_DIR}/src/uvrpc_async.c
    ${UV_DIR}/src/uvrpc_primitives.c
    ${UV_DIR}/src/uvrpc_config.c
    ${UV_DIR}/src/uvrpc_strerror.c
    ${UV_DIR}/src/uvasync.c
    ${UV_DIR}/src/uvbus.c
    ${UV_DIR}/src/uvbus_transport_tcp.c
    ${UV_DIR}/src/uvbus_transport_udp.c
    ${UV_DIR}/src/uvbus_transport_ipc.c
    ${UV_DIR}/src/uvbus_transport_inproc.c
    ${UV_DIR}/src/uvbus_transport_sameloop.c
    ${UV_DIR}/src/uvbus_loop_registry.c
)
target_include_directories(qzos_uvrpc PUBLIC
    ${UV_DIR}/include
    ${QZ_VENDOR_DIR}/uvrpc-generated
)
target_include_directories(qzos_uvrpc PRIVATE
    ${UV_DIR}/src
    ${QZ_VENDOR_DIR}/uthash/src
)
target_compile_definitions(qzos_uvrpc PUBLIC UVRPC_DEFAULT_ALLOCATOR=0)
target_link_libraries(qzos_uvrpc PUBLIC uv_a qzos_flatcc)

# ----------------------------------------------------------------------------
# Fusion Pixel 字体（1bpp 位图；见 brain qzos-ui-architecture 字体时间线）
# ----------------------------------------------------------------------------
file(GLOB QZOS_FONT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fonts/fusion_pixel_*.c)
if(NOT QZOS_FONT_SOURCES)
    message(FATAL_ERROR "no font sources — run scripts/gen-fonts.sh first")
endif()
add_library(qzos_fonts STATIC ${QZOS_FONT_SOURCES})
target_link_libraries(qzos_fonts PUBLIC lvgl)

# ----------------------------------------------------------------------------
# qzos-host：uv loop 泵 + LVGL 显示/输入 + JSON UI 桥 + uvrpc 服务面
#
# CONFIGURE_DEPENDS 不能省：没有它，新增 src/*.c 后构建照常"成功"，只是
# 新文件根本没进二进制——keymap.c 就这样悄悄漏过一次（链接期才报 undefined）。
# ----------------------------------------------------------------------------
file(GLOB QZOS_HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(qzos-host ${QZOS_HOST_SOURCES})
target_include_directories(qzos-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qzos-host PRIVATE
    qz_full          # qzjs + uv_a + cjson（传递闭包）
    lvgl
    qzos_fonts
    qzos_uvrpc
    qzos_flatcc
)

# qzjs-rt（ISOLATED 运行时进程）与宿主同目录部署；显式依赖保证一起构建
if(TARGET qz_rt)
    add_dependencies(qzos-host qz_rt)
    # 宿主经 QZ_RT_SERVER（main.c QZ_RT_SERVER_PATH）定位运行时：
    # 构建树里直接指向产物，部署时见 scripts/deploy-os.sh（拷同目录）。
    target_compile_definitions(qzos-host PRIVATE
        QZ_RT_SERVER_PATH="$<TARGET_FILE:qz_rt>")
    add_custom_command(TARGET qzos-host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:qz_rt> $<TARGET_FILE_DIR:qzos-host>/qzjs-rt
        COMMENT "copy qzjs-rt alongside qzos-host")
endif()

# JS 侧（shell/ui/应用）拷到输出目录，随宿主一起部署到 /storage
add_custom_command(TARGET qzos-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/js $<TARGET_FILE_DIR:qzos-host>/js
    COMMENT "copy os/js -> $<TARGET_FILE_DIR:qzos-host>/js")
