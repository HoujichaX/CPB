#pragma once
#include <vector>
#include <cstdint>
#include <string>

using Byte = uint8_t;

#pragma pack(push,1)
struct RVAC_Header {
    char magic[8] = {'R','V','A','C','P','R','O',0};
    uint16_t version = 4;           // v4: pluggable compression
    uint32_t blob_count = 0;
    uint64_t total_size = 0;
    uint64_t compressed_size = 0;
    uint64_t seed = 0;
    uint8_t  compression_method = 0x01; // CompressMethod enum (ZSTD=0x01)
    uint8_t  compression_level  = 3;
    uint8_t  _pad[6]            = {};   // 将来拡張用
    uint8_t dsl_hash[32]        = {};
    uint8_t original_hash[32]   = {};
};
#pragma pack(pop)

// 復元検証結果
struct VerifyResult {
    bool    hash_match      = false; // SHA-256 完全一致
    double  byte_match_rate = 0.0;   // バイト一致率 (0.0〜1.0)
    size_t  expected_size   = 0;     // ヘッダーに記録されたサイズ
    size_t  actual_size     = 0;     // 実際に復元できたサイズ
    int     repair_failed   = 0;     // RS 修復失敗 Blob 数
    char    detail[256]     = {};    // 人間向けメッセージ

    bool ok() const { return hash_match && byte_match_rate >= 1.0; }
};

struct Blob {
    std::vector<Byte> data;
};

// ============================================================
// RS 符号化パラメータ (block + parity <= 255)
// ============================================================
// Blob 用: RS(255, 223)  最大 floor(32/2)=16 バイト誤り訂正
//   encode後サイズ = 8 + ceil(data/223)*255
//   1MB blob → ~1.14MB  (フレーム容量 ~1.98MB 以内)
constexpr size_t RS_BLOB_BLOCK  = 223;
constexpr size_t RS_BLOB_PARITY = 32;

// DSL 用: RS(255, 127)  最大 floor(128/2)=64 バイト誤り訂正 (高冗長化)
//   DSL は小さいので高冗長化しても Frame0 に余裕で収まる
constexpr size_t RS_DSL_BLOCK  = 127;
constexpr size_t RS_DSL_PARITY = 128;

std::vector<uint8_t> load_bytes(const std::string& path);
void save_bytes(const std::string& path, const std::vector<uint8_t>& data);

std::vector<Blob> split_into_blobs(const std::vector<uint8_t>& data);

// 最小版DSL: OUTPUT + RAW + FF
std::vector<uint8_t> build_dsl(const std::vector<Blob>& blobs, size_t total_size);