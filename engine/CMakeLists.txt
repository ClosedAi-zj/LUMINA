# LUMINA 引擎构建定义：C++17 + LibRaw 静态库 + SQLite amalgamation + stb
cmake_minimum_required(VERSION 3.20)
project(LUMINA_ENGINE LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "build type" FORCE)
endif()

# ---- 第三方依赖路径（thirdparty 只读） ----
set(LUMINA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(THIRDPARTY_DIR "${LUMINA_ROOT}/thirdparty" CACHE PATH "thirdparty root")

if(NOT EXISTS "${THIRDPARTY_DIR}/libraw/lib/libraw.a")
  message(FATAL_ERROR "missing libraw.a under ${THIRDPARTY_DIR}/libraw/lib")
endif()

# ---- 全局宏：Win10 API + LibRaw 单线程 + SQLite FTS5 ----
add_definitions(-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
add_definitions(-DLIBRAW_NOTHREADS)
add_definitions(-DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1)
add_definitions(-DSQLITE_ENABLE_JSON1 -DSQLITE_DEFAULT_FOREIGN_KEYS=1)
add_definitions(-D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE)

# MinGW 静态链接运行时，避免目标机缺 dll
set(LUMINA_LINK_FLAGS "-static-libgcc -static-libstdc++")

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${THIRDPARTY_DIR}"
  "${THIRDPARTY_DIR}/libraw"
  "${CMAKE_CURRENT_BINARY_DIR}"
)

# ---- schema.sql -> C++ 原始字符串（保证 schema.sql 是唯一事实源） ----
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/db/schema.sql" LUMINA_SCHEMA_SQL)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/schema_sql.inc"
     "// 由 CMake 从 src/db/schema.sql 生成，勿手工编辑\n"
     "static const char* kSchemaSql = R\"SCHEMA_RAW(\n${LUMINA_SCHEMA_SQL}\n)SCHEMA_RAW\";\n")

set(LUMINA_SOURCES
  src/json/json_value.cpp
  src/core/config.cpp
  src/core/logger.cpp
  src/core/job_manager.cpp
  src/core/md5.cpp
  src/core/str_util.cpp
  src/core/models.cpp
  src/rpc/frame.cpp
  src/rpc/rpc_server.cpp
  src/rpc/dispatcher.cpp
  src/rpc/methods.cpp
  src/db/database.cpp
  src/db/photo_dao.cpp
  src/db/fts_indexer.cpp
  src/db/album_dao.cpp
  src/db/health_dao.cpp
  src/library/query_engine.cpp
  src/map/map_service.cpp
  src/library/folder_tree.cpp
  src/library/smart_collections.cpp
  src/imaging/imaging.cpp
  src/imaging/image_buffer.cpp
  src/imaging/resample.cpp
  src/imaging/output_encode.cpp
  src/plan/op_table.cpp
  src/plan/geometry.cpp
  src/plan/cache_key.cpp
  src/plan/render_plan.cpp
  src/graph/node_graph.cpp
  src/graph/serialize.cpp
  src/plan/ops/basic.cpp
  src/plan/ops/curve.cpp
  src/plan/ops/color.cpp
  src/plan/ops/calibration.cpp
  src/plan/ops/detail.cpp
  src/plan/ops/lens.cpp
  src/plan/ops/geometry.cpp
  src/plan/ops/effects.cpp
  src/plan/ops/mask.cpp
  src/thumb/thumbnail_service.cpp
  src/raw/raw_decoder.cpp
  src/raw/embedded_proxy.cpp
  src/metadata/exif_reader.cpp
  src/metadata/video_probe.cpp
  src/xmp/xmp_document.cpp
  src/xmp/xmp_mapper.cpp
  src/xmp/xmp_service.cpp
  src/xmp/migrator.cpp
  src/edit/edit_session.cpp
  src/edit/history.cpp
  src/edit/virtual_copy.cpp
  src/import/scanner.cpp
  src/import/importer.cpp
  src/import/rename.cpp
  src/health/scanner.cpp
  src/health/watcher.cpp
  src/health/backup.cpp
  src/health/db_backup.cpp
  src/mock/generator.cpp
  src/core/jpeg_encoder.cpp
  src/export/exporter.cpp
  src/export/text_raster.cpp
  src/gallery/gallery_service.cpp
  src/specialty/hdr_service.cpp
  src/specialty/denoise_service.cpp
  src/timelapse/timelapse_service.cpp
)

# sqlite3.c 必须由 gcc（C 编译器）编译；附加 FTS5 与 Win10 宏
set(LUMINA_SQLITE_SRC "${THIRDPARTY_DIR}/sqlite/sqlite3.c")
set_source_files_properties(${LUMINA_SQLITE_SRC} PROPERTIES
  LANGUAGE C
  COMPILE_FLAGS "-DSQLITE_ENABLE_FTS5 -D_WIN32_WINNT=0x0A00 -DSQLITE_THREADSAFE=1 -O1")

add_library(lumina_core STATIC ${LUMINA_SOURCES} ${LUMINA_SQLITE_SRC})

target_link_libraries(lumina_core PUBLIC
  "${THIRDPARTY_DIR}/libraw/lib/libraw.a"
  ws2_32 m gdi32 user32)

# ---- 主程序 ----
add_executable(lumina-engine src/main.cpp)
target_link_libraries(lumina-engine PRIVATE lumina_core)
set_target_properties(lumina-engine PROPERTIES
  LINK_FLAGS "${LUMINA_LINK_FLAGS}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---- 测试 ----
enable_testing()
add_executable(test_xmp_roundtrip tests/test_xmp_roundtrip.cpp)
target_link_libraries(test_xmp_roundtrip PRIVATE lumina_core)
set_target_properties(test_xmp_roundtrip PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")

add_executable(test_rpc_smoke tests/test_rpc_smoke.cpp)
target_link_libraries(test_rpc_smoke PRIVATE lumina_core)
set_target_properties(test_rpc_smoke PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")

add_executable(test_query_perf tests/test_query_perf.cpp)
target_link_libraries(test_query_perf PRIVATE lumina_core)
set_target_properties(test_query_perf PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")

# GPS 清除回归：createNew -> set -> parse -> remove 路径（map.clearPhotoLocation 依赖）
add_executable(test_xmp_gps tests/test_xmp_gps.cpp)
target_link_libraries(test_xmp_gps PRIVATE lumina_core)
set_target_properties(test_xmp_gps PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")

# ToneCurve 文本编解码回归（v1.2.0 P0：parseCurve 全零修复）
add_executable(test_xmp_curve tests/test_xmp_curve.cpp)
target_link_libraries(test_xmp_curve PRIVATE lumina_core)
set_target_properties(test_xmp_curve PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")

add_test(NAME xmp_roundtrip COMMAND test_xmp_roundtrip)
add_test(NAME xmp_gps COMMAND test_xmp_gps)
add_test(NAME xmp_curve COMMAND test_xmp_curve)

# ---- M0：16bit 地基 ----
add_executable(test_image16 tests/test_image16.cpp)
target_link_libraries(test_image16 PRIVATE lumina_core)
set_target_properties(test_image16 PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME image16 COMMAND test_image16)

add_executable(test_raw16 tests/test_raw16.cpp)
target_link_libraries(test_raw16 PRIVATE lumina_core)
set_target_properties(test_raw16 PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME raw16 COMMAND test_raw16)

add_executable(test_op_table tests/test_op_table.cpp)
target_link_libraries(test_op_table PRIVATE lumina_core)
set_target_properties(test_op_table PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME op_table COMMAND test_op_table)

# T0.4：几何单应 + 一次重采样
add_executable(test_geometry16 tests/test_geometry16.cpp)
target_link_libraries(test_geometry16 PRIVATE lumina_core)
set_target_properties(test_geometry16 PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME geometry16 COMMAND test_geometry16)

# T0.5：三键缓存 + Plan
add_executable(test_plan_cache tests/test_plan_cache.cpp)
target_link_libraries(test_plan_cache PRIVATE lumina_core)
set_target_properties(test_plan_cache PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME plan_cache COMMAND test_plan_cache)

# T0.6：parity harness（量化管线 vs double 参考；报表落 build 目录）
configure_file(tests/parity/params.json ${CMAKE_CURRENT_BINARY_DIR}/parity_params.json COPYONLY)
add_executable(parity_harness tests/parity/harness.cpp)
target_link_libraries(parity_harness PRIVATE lumina_core)
set_target_properties(parity_harness PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME parity
  COMMAND parity_harness
          ${CMAKE_CURRENT_BINARY_DIR}/parity_params.json
          ${CMAKE_BINARY_DIR}/parity-report.json)

# T0.7：8bit TPDF 输出编码 + 缓存 v2
add_executable(test_output_encode tests/test_output_encode.cpp)
target_link_libraries(test_output_encode PRIVATE lumina_core)
set_target_properties(test_output_encode PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME output_encode COMMAND test_output_encode)

# T1.1：NodeGraph 数据结构 + JSON 编解码
add_executable(test_node_graph tests/test_node_graph.cpp)
target_link_libraries(test_node_graph PRIVATE lumina_core)
set_target_properties(test_node_graph PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
add_test(NAME node_graph COMMAND test_node_graph)

# M0 ΔE 基线工具（不入 ctest：依赖本机 RW2 样本；由 scripts/ci/deltae.py 编排）
add_executable(deltae_baseline tests/color/delta_e_baseline.cpp)
target_link_libraries(deltae_baseline PRIVATE lumina_core)
set_target_properties(deltae_baseline PROPERTIES LINK_FLAGS "${LUMINA_LINK_FLAGS}")
