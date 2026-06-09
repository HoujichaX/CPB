#include "header.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

// ------------------------------------------------------------
// 簡易ハッシュ（OpenSSLなし版）
// ※とりあえず動かす用（後で差し替え可能）
// ------------------------------------------------------------
void compute_hash(const std::vector<uint8_t>& data, uint8_t out[32])
{
    // 超簡易：XORベース
    // とりあえず動作確認用（本番はSHA256に戻す）

    std::memset(out, 0, 32);

    for (size_t i = 0; i < data.size(); ++i) {
        out[i % 32] ^= data[i];
    }
}

// ------------------------------------------------------------
// ヘッダー → バイト列
// ------------------------------------------------------------
std::vector<uint8_t> serialize_header(const RVAC_Header& h)
{
    std::vector<uint8_t> out(sizeof(RVAC_Header));
    std::memcpy(out.data(), &h, sizeof(RVAC_Header));
    return out;
}

// ------------------------------------------------------------
// バイト列 → ヘッダー
// ------------------------------------------------------------
RVAC_Header deserialize_header(const std::vector<uint8_t>& buf)
{
    if (buf.size() < sizeof(RVAC_Header))
        throw std::runtime_error("Header buffer too small");

    RVAC_Header h;
    std::memcpy(&h, buf.data(), sizeof(RVAC_Header));

    // magic チェック
    if (std::memcmp(h.magic, "RVACPRO", 7) != 0)
        throw std::runtime_error("Invalid RVAC header magic");

    return h;
}

// ------------------------------------------------------------
// デバッグ表示
// ------------------------------------------------------------
void print_header(const RVAC_Header& h)
{
    std::cout << "=== RVAC Header ===\n";
    std::cout << "magic: " << h.magic << "\n";
    std::cout << "version: " << h.version << "\n";
    std::cout << "blob_count: " << h.blob_count << "\n";
    std::cout << "total_size: " << h.total_size << "\n";
    std::cout << "compressed_size: " << h.compressed_size << "\n";
    std::cout << "seed: " << h.seed << "\n";
    std::cout << "dsl_hash: ";

    for (int i = 0; i < 32; i++)
        printf("%02x", h.dsl_hash[i]);

    std::cout << "\n====================\n";
}