#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "container.hpp"

// ヘッダーの読み書き
void compute_hash(const std::vector<uint8_t>& data, uint8_t out[32]);

// Frame0 に埋め込むヘッダーをバイト列に変換
std::vector<uint8_t> serialize_header(const RVAC_Header& h);

// バイト列からヘッダーを復元
RVAC_Header deserialize_header(const std::vector<uint8_t>& buf);

// デバッグ用（任意）
void print_header(const RVAC_Header& h);