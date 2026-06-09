#pragma once
#include <vector>
#include <cstdint>
#include <string>

// ============================================================
// CPB Level 2 — プラガブル圧縮インターフェース
// ============================================================

enum class CompressMethod : uint8_t {
    NONE        = 0x00, // パススルー (無圧縮)
    ZSTD        = 0x01, // 汎用高圧縮 (Zstd)
    LZ4         = 0x02, // 超高速 (リアルタイム向け)
    DELTA       = 0x03, // 差分符号化 (時系列/座標)
    RLE         = 0x04, // ランレングス (単色画像)
    MSZIP       = 0x05, // DEFLATE/ZIP互換 (Windows標準)
    XPRESS      = 0x06, // XPRESS LZ (高速・中圧縮)
    XPRESS_HUFF = 0x07, // XPRESS+Huffman (バランス型)
    LZMS        = 0x08, // LZMS (最高圧縮・低速)
    AUTO        = 0xFF, // エントロピー解析で自動選択
};

// 圧縮方式 → 文字列
inline const char* method_name(CompressMethod m) {
    switch(m){
    case CompressMethod::NONE:  return "NONE";
    case CompressMethod::ZSTD:  return "ZSTD";
    case CompressMethod::LZ4:   return "LZ4";
    case CompressMethod::DELTA: return "DELTA";
    case CompressMethod::RLE:   return "RLE";
    case CompressMethod::AUTO:  return "AUTO";
    default:                    return "UNKNOWN";
    }
}

// 圧縮・解凍API
std::vector<uint8_t> cpb_compress(
    const std::vector<uint8_t>& in,
    CompressMethod method,
    int level = 3);

std::vector<uint8_t> cpb_decompress(
    const std::vector<uint8_t>& in,
    CompressMethod method,
    size_t orig_hint = 0);

// エントロピー解析による自動選択
CompressMethod cpb_auto_detect(const std::vector<uint8_t>& data);
