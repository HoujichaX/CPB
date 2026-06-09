#pragma once
#include <vector>
#include <cstdint>
#include <string>

// ============================================================
// CPB Protection Layer
//
// "どのコーデックで守るか" を一枚のインターフェースで抽象化。
// CPBConfig の protection_codec フィールドで選択。
//
// v1: REED_SOLOMON   — 実装済み
// v2: XOR_PARITY     — 今回実装
// v3: FOUNTAIN_LT    — 今回実装
// v4: CAUCHY_RS      — 将来
// ============================================================

enum class ProtectionCodec : uint8_t {
    NONE         = 0x00, // 保護なし
    XOR_PARITY   = 0x01, // XOR パリティ (軽量・高速)
    REED_SOLOMON = 0x02, // RS(255,k) (デフォルト)
    CAUCHY_RS    = 0x03, // Cauchy-RS (巨大データ向け, 将来)
    FOUNTAIN_LT  = 0x04, // LT Fountain code (4D散布と相性最良)
    RAPTOR       = 0x05, // Raptor (Fountain上位版, 将来)
    LDPC_ERASURE = 0x06, // LDPC 消失符号 (将来)
    BCH          = 0x07, // BCH ビット誤り訂正 (将来)
};

inline const char* codec_name(ProtectionCodec c) {
    switch(c){
    case ProtectionCodec::NONE:         return "NONE";
    case ProtectionCodec::XOR_PARITY:   return "XOR_PARITY";
    case ProtectionCodec::REED_SOLOMON: return "REED_SOLOMON";
    case ProtectionCodec::CAUCHY_RS:    return "CAUCHY_RS";
    case ProtectionCodec::FOUNTAIN_LT:  return "FOUNTAIN_LT";
    case ProtectionCodec::RAPTOR:       return "RAPTOR";
    case ProtectionCodec::LDPC_ERASURE: return "LDPC_ERASURE";
    case ProtectionCodec::BCH:          return "BCH";
    default:                            return "UNKNOWN";
    }
}

// ============================================================
// 統一エンコード/デコードAPI
// ============================================================

// エンコードパラメータ
struct ProtectionParams {
    ProtectionCodec codec    = ProtectionCodec::REED_SOLOMON;

    // RS / Cauchy-RS
    size_t rs_block          = 223;
    size_t rs_parity         = 32;

    // XOR parity
    size_t xor_stripe        = 1024 * 1024; // ストライプサイズ (bytes)

    // Fountain/LT
    size_t lt_k              = 0;    // 0=auto (ceil(data/symbol_size))
    size_t lt_symbol_size    = 1024; // シンボルサイズ (bytes)
    double lt_overhead       = 0.10; // 冗長率 (0.10 = 10%余分に生成)
    uint64_t lt_seed         = 42;   // 乱数シード (再現性確保)

    // プリセット
    static ProtectionParams rs_standard(){
        ProtectionParams p; p.codec=ProtectionCodec::REED_SOLOMON;
        p.rs_block=223; p.rs_parity=32; return p;
    }
    static ProtectionParams rs_max(){
        ProtectionParams p; p.codec=ProtectionCodec::REED_SOLOMON;
        p.rs_block=127; p.rs_parity=128; return p;
    }
    static ProtectionParams xor_parity(size_t stripe=1024*1024){
        ProtectionParams p; p.codec=ProtectionCodec::XOR_PARITY;
        p.xor_stripe=stripe; return p;
    }
    static ProtectionParams fountain(size_t sym=1024, double overhead=0.10){
        ProtectionParams p; p.codec=ProtectionCodec::FOUNTAIN_LT;
        p.lt_symbol_size=sym; p.lt_overhead=overhead; return p;
    }
};

// エンコード結果: 符号化済みシンボル列
struct ProtectionEncoded {
    ProtectionCodec codec;
    ProtectionParams params;
    size_t           original_size; // 元データサイズ
    size_t           k;             // ソースシンボル数
    size_t           n;             // 符号化シンボル数 (k <= n)

    // シンボル列: symbols[i] = i番目の符号化シンボル
    // RS:      [parity][data] のコードワード列
    // XOR:     データストライプ列 + パリティストライプ
    // Fountain: 任意のn個のシンボル (どの組み合わせでも復元可)
    std::vector<std::vector<uint8_t>> symbols;

    // Fountain: 各シンボルの生成情報 (どのソースブロックをXORしたか)
    std::vector<std::vector<uint32_t>> lt_neighbors; // symbols[i]の隣接ソース
};

// エンコード
ProtectionEncoded protect_encode(
    const std::vector<uint8_t>& data,
    const ProtectionParams& params);

// デコード
// available_symbols: 受信できたシンボルのインデックス→データ
// (欠損シンボルは渡さない)
std::vector<uint8_t> protect_decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& available);

// 何シンボル受信すれば復元できるか (理論値)
size_t min_symbols_for_recovery(const ProtectionEncoded& enc);
