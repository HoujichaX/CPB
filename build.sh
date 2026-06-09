#!/bin/bash
# CPB ビルドスクリプト (cmake不要版)
# 使い方: bash build.sh [--with-zstd] [--with-ffmpeg]

set -e
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -I."
DEFS="-DCPB_NO_ZSTD -DNO_FRAME_IO"
LIBS=""

# Zstd オプション
if [[ "$*" == *--with-zstd* ]]; then
    DEFS="${DEFS/-DCPB_NO_ZSTD/}"
    LIBS="$LIBS -lzstd"
    echo "[CPB] Zstd: enabled"
else
    echo "[CPB] Zstd: stub (LZ4 fallback)"
fi

# FFmpeg オプション
if [[ "$*" == *--with-ffmpeg* ]]; then
    DEFS="$DEFS -DCPB_REAL_FFMPEG"
    LIBS="$LIBS $(pkg-config --libs libavcodec libavformat libavutil libswscale)"
    echo "[CPB] FFmpeg: enabled"
fi

SRCS="rs_codec.cpp header.cpp container.cpp dsl_vm.cpp frame_index.cpp \
      protection_layer.cpp compress_impl.cpp genre_dsl.cpp genre_dsl_vm.cpp \
      fourd_map.cpp gen_codec.cpp cpb_dict_protocol.cpp dict_evolution.cpp \
      dict_share.cpp search_api.cpp layer_pipeline.cpp rvac.cpp"

echo "[CPB] Building cpb_cli..."
$CXX $FLAGS $DEFS $SRCS main.cpp -o cpb_cli $LIBS
echo "[CPB] Built: cpb_cli"

echo "[CPB] Building tests..."
for t in run_tests test_dsl_lz77 test_genre_dsl test_verify test_compress \
          test_frame_index test_fourd test_search_api test_config test_protection \
          test_dict_protocol test_dict_evolution test_pipeline \
          test_stub_integration test_phase3 test_l5_learning test_cli_e2e; do
    if [ -f "${t}.cpp" ]; then
        $CXX $FLAGS $DEFS -DCPB_NO_ZSTD -DNO_FRAME_IO ${t}.cpp -o ${t}
        echo "  built: $t"
    fi
done

echo ""
echo "[CPB] Build complete! Run: ctest or ./cpb_cli --help"
