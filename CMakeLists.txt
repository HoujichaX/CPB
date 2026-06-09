cmake_minimum_required(VERSION 3.20)
project(cpb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# S1: Zstd — 自動検出。なければスタブにフォールバック
# ============================================================
find_package(zstd CONFIG QUIET)
if(zstd_FOUND)
    set(CPB_HAS_ZSTD TRUE)
    message(STATUS "[CPB] Zstd: FOUND — real compression enabled")
else()
    set(CPB_HAS_ZSTD FALSE)
    message(STATUS "[CPB] Zstd: not found — stub fallback (vcpkg install zstd)")
endif()

# ============================================================
# S2: OpenCV — 自動検出。なければ soft frame_io にフォールバック
# ============================================================
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    set(CPB_HAS_OPENCV TRUE)
    message(STATUS "[CPB] OpenCV: FOUND v${OpenCV_VERSION} — hardware frame I/O enabled")
else()
    set(CPB_HAS_OPENCV FALSE)
    message(STATUS "[CPB] OpenCV: not found — soft frame I/O (vcpkg install opencv4)")
endif()

# ============================================================
# S3/S4: FFmpeg — オプション。-DCPB_REAL_FFMPEG=ON で有効化
# ============================================================
option(CPB_REAL_FFMPEG "Build with real FFmpeg backends (S3/S4)" OFF)
if(CPB_REAL_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
        libavcodec libavformat libavutil libswscale
    )
    message(STATUS "[CPB] FFmpeg: ENABLED — FFV1/MKV + H264/MP4 backends active")
else()
    message(STATUS "[CPB] FFmpeg: disabled (stub) — use -DCPB_REAL_FFMPEG=ON to enable")
endif()

find_package(GTest CONFIG QUIET)

# ============================================================
# CPB コアライブラリ
# ============================================================
set(CPB_SOURCES
    rs_codec.cpp
    header.cpp
    container.cpp
    dsl_vm.cpp
    frame_index.cpp
    protection_layer.cpp
    compress_impl.cpp
    genre_dsl.cpp
    genre_dsl_vm.cpp
    fourd_map.cpp
    gen_codec.cpp
    cpb_dict_protocol.cpp
    dict_evolution.cpp
    dict_share.cpp
    search_api.cpp
    layer_pipeline.cpp
    rvac.cpp
)

# S2: OpenCVがあればframe_io.cpp、なければframe_io_soft.cpp
if(CPB_HAS_OPENCV)
    list(APPEND CPB_SOURCES frame_io.cpp)
else()
    list(APPEND CPB_SOURCES frame_io_soft.cpp)
endif()

# S1: ZstdがあればZstdラッパーも追加
if(CPB_HAS_ZSTD)
    list(APPEND CPB_SOURCES zstd_wrap.cpp)
endif()

add_library(cpb_core STATIC ${CPB_SOURCES})
target_include_directories(cpb_core PUBLIC .)

# リンク: Zstd
if(CPB_HAS_ZSTD)
    target_link_libraries(cpb_core PUBLIC zstd::libzstd_shared)
else()
    target_compile_definitions(cpb_core PUBLIC CPB_NO_ZSTD)
endif()

# リンク: OpenCV
if(CPB_HAS_OPENCV)
    target_link_libraries(cpb_core PUBLIC ${OpenCV_LIBS})
else()
    target_compile_definitions(cpb_core PUBLIC NO_FRAME_IO)
endif()

# リンク: FFmpeg
if(CPB_REAL_FFMPEG)
    target_compile_definitions(cpb_core PUBLIC CPB_REAL_FFMPEG)
    target_include_directories(cpb_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(cpb_core PUBLIC ${FFMPEG_LIBRARIES})
endif()

# ============================================================
# メイン実行バイナリ
# ============================================================
add_executable(cpb main.cpp)

# ============================================================
# GUI アプリ (Windows専用)
# ============================================================
if(WIN32)
    add_executable(cpb_gui WIN32 cpb_gui.cpp)
    target_compile_definitions(cpb_gui PRIVATE
        CPB_NO_ZSTD
        NO_FRAME_IO
        WIN32
        _WINDOWS
        UNICODE
        _UNICODE
    )
    target_link_libraries(cpb_gui PRIVATE
        comctl32 shell32 user32 gdi32
    )
    message(STATUS "[CPB] GUI: cpb_gui.exe (Windows)")
endif()

target_link_libraries(cpb PRIVATE cpb_core)

# ============================================================
# ユニットテスト — スタブ付き単体コンパイル
# ============================================================
enable_testing()

set(TEST_SUITES
    test_rs_codec
    test_dsl_lz77
    test_genre_dsl
    test_verify
    test_compress
    test_frame_index
    test_fourd
    test_search_api
    test_config
    test_protection
    test_dict_protocol
    test_dict_evolution
    test_pipeline
    test_stub_integration
    test_phase3
    test_l5_learning
    test_cli_e2e
    real_data_test
)

foreach(SUITE ${TEST_SUITES})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SUITE}.cpp")
        add_executable(${SUITE} ${SUITE}.cpp)
        target_include_directories(${SUITE} PRIVATE .)
        # テストは常にスタブで動く
        target_compile_definitions(${SUITE} PRIVATE
            NO_FRAME_IO
            CPB_NO_ZSTD
        )
        add_test(NAME ${SUITE} COMMAND ${SUITE})
    endif()
endforeach()

# ============================================================
# 本番結合テスト — 実ライブラリ使用 (全スタブ除去)
# ============================================================
option(CPB_INTEGRATION_TEST "Build integration tests" ON)
if(CPB_INTEGRATION_TEST AND CPB_HAS_ZSTD)
    add_executable(integration_test integration_test.cpp)
    target_link_libraries(integration_test PRIVATE cpb_core)
    add_test(NAME integration_test COMMAND integration_test)
    message(STATUS "[CPB] Integration test: ENABLED")
else()
    message(STATUS "[CPB] Integration test: skipped (Zstd required)")
endif()

# ============================================================
# ビルドサマリー
# ============================================================
message(STATUS "")
message(STATUS "┌─────────────────────────────────────────┐")
message(STATUS "│  CPB — Cocoa Powder Bottle               │")
message(STATUS "│  Invisible Data Architecture             │")
message(STATUS "├─────────────────────────────────────────┤")
message(STATUS "│  Zstd   : ${CPB_HAS_ZSTD}")
message(STATUS "│  OpenCV : ${CPB_HAS_OPENCV}")
message(STATUS "│  FFmpeg : ${CPB_REAL_FFMPEG}")
message(STATUS "└─────────────────────────────────────────┘")
message(STATUS "")
