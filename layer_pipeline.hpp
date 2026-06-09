#pragma once
#include "cpb_config.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <functional>

// ============================================================
// CPB Layer Pipeline — レイヤー順序プロファイル
//
// 順序自体がステガノ要素になる。
// profile_id を知らないと復元できない。
//
// 標準順序 (圧縮率優先):
//   Input → L5 → L3 → L2 → L1 → L4a → L4b → Base
//
// 防御優先:
//   Input → L3 → L2 → L4a(shuffle) → L1 → L4b(place) → Base
//
// AI通信パケット:
//   Input → L5 → L3 → L1 → semantic header → Base
// ============================================================

// ── レイヤーID ──
enum class LayerID : uint8_t {
    BASE  = 0x00,  // 動画コンテナ (常に最後)
    L1    = 0x01,  // Protection (RS/Fountain/Noise)
    L2    = 0x02,  // Generic Compression (Zstd/LZ4/Delta/RLE)
    L3    = 0x03,  // Genre DSL (構造的圧縮)
    L4A   = 0x04,  // 4D Semantic Shuffle (意味順序を壊す)
    L4B   = 0x05,  // 4D Frame Placement (動画内配置)
    L5       = 0x06,  // Generation Dict: lookup-only (高速・辞書参照のみ)
    FIDX     = 0x07,  // Frame Index (検索インデックス)
    L5_LEARN = 0x08,  // Generation Dict: learning (低速・L3出力を辞書に学習)
};

inline const char* layer_name(LayerID l) {
    switch(l){
    case LayerID::BASE:  return "Base";
    case LayerID::L1:    return "L1:Protection";
    case LayerID::L2:    return "L2:Compression";
    case LayerID::L3:    return "L3:GenreDSL";
    case LayerID::L4A:   return "L4a:Shuffle";
    case LayerID::L4B:   return "L4b:Placement";
    case LayerID::L5:    return "L5:GenDict";
    case LayerID::FIDX:     return "FIDX:Search";
    case LayerID::L5_LEARN: return "L5:Learning";
    default:                return "Unknown";
    }
}

// ── パイプラインプロファイル ──
enum class PipelineProfile : uint8_t {
    STANDARD      = 0x01,  // 標準 (圧縮率優先)
    DEFENSE       = 0x02,  // 防御優先 (L4aでシャッフル後にL1)
    STEGO         = 0x03,  // ステガノ優先
    ARCHIVE       = 0x04,  // 公文書 (圧縮なし)
    AI_PACKET     = 0x05,  // AI通信パケット (軽量・高速復元)
    CUSTOM        = 0xFF,  // カスタム順序
    LEARN         = 0x06,  // 学習モード (L3→L5_LEARN、辞書育成)
};

struct LayerPipeline {
    PipelineProfile profile = PipelineProfile::STANDARD;
    std::vector<LayerID> encode_order; // エンコード時のレイヤー適用順
    // デコードは encode_order の逆順に自動で行う
    uint64_t profile_seed = 0; // 順序自体の秘匿シード

    // ── プリセット ──
    static LayerPipeline standard() {
        LayerPipeline p; p.profile=PipelineProfile::STANDARD;
        p.encode_order = {LayerID::L5, LayerID::L3, LayerID::L2,
                          LayerID::L1, LayerID::L4B, LayerID::FIDX};
        return p;
    }
    static LayerPipeline defense() {
        LayerPipeline p; p.profile=PipelineProfile::DEFENSE;
        // L4aでシャッフルしてからL1で保護
        p.encode_order = {LayerID::L3, LayerID::L2, LayerID::L4A,
                          LayerID::L1, LayerID::L4B, LayerID::FIDX};
        return p;
    }
    static LayerPipeline stego() {
        LayerPipeline p; p.profile=PipelineProfile::STEGO;
        p.encode_order = {LayerID::L2, LayerID::L1, LayerID::L4A,
                          LayerID::L4B};
        return p;
    }
    static LayerPipeline archive() {
        LayerPipeline p; p.profile=PipelineProfile::ARCHIVE;
        // 圧縮なし・高保護・検索可能
        p.encode_order = {LayerID::L1, LayerID::L4B, LayerID::FIDX};
        return p;
    }
    static LayerPipeline ai_packet() {
        LayerPipeline p; p.profile=PipelineProfile::AI_PACKET;
        // L5辞書 → L3構造圧縮 → L1保護 → FIDX (高速部分取得)
        p.encode_order = {LayerID::L5, LayerID::L3, LayerID::L1,
                          LayerID::FIDX};
        return p;
    }
    // 学習モード: L3 → L5_LEARN → L2 → L1 → L4b
    // L3が生成したDSLをL5_LEARNが辞書に学習。辞書が育つほど次回のL5ヒット率が上がる。
    static LayerPipeline learn() {
        LayerPipeline p; p.profile=PipelineProfile::LEARN;
        p.encode_order = {LayerID::L3, LayerID::L5_LEARN, LayerID::L2,
                          LayerID::L1, LayerID::L4B, LayerID::FIDX};
        return p;
    }
    // 高速モード: L5ヒットならL3スキップ (standard と同じ順序、辞書育成なし)
    // CPBConfig::learning = false の場合はこちらが自動選択
    static LayerPipeline fast() { return standard(); }

    static LayerPipeline custom(std::vector<LayerID> order, uint64_t seed=0) {
        LayerPipeline p; p.profile=PipelineProfile::CUSTOM;
        p.encode_order=std::move(order); p.profile_seed=seed;
        return p;
    }

    // デコード順序 (エンコードの逆)
    std::vector<LayerID> decode_order() const {
        auto r = encode_order;
        std::reverse(r.begin(), r.end());
        return r;
    }

    // 説明文
    std::string describe() const {
        std::string s;
        switch(profile){
        case PipelineProfile::STANDARD:  s="STANDARD  "; break;
        case PipelineProfile::DEFENSE:   s="DEFENSE   "; break;
        case PipelineProfile::STEGO:     s="STEGO     "; break;
        case PipelineProfile::ARCHIVE:   s="ARCHIVE   "; break;
        case PipelineProfile::AI_PACKET: s="AI_PACKET "; break;
        default:                          s="CUSTOM    "; break;
        }
        s += "encode: ";
        for(size_t i=0; i<encode_order.size(); ++i){
            if(i) s += " → ";
            s += layer_name(encode_order[i]);
        }
        return s;
    }

    // シリアライズ (CPBヘッダーに記録)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> b;
        b.push_back((uint8_t)profile);
        b.push_back((uint8_t)encode_order.size());
        for(auto l:encode_order) b.push_back((uint8_t)l);
        for(int i=0;i<8;++i)
            b.push_back((uint8_t)((profile_seed>>(i*8))&0xFF));
        return b;
    }
    static LayerPipeline deserialize(const std::vector<uint8_t>& b) {
        LayerPipeline p;
        if(b.size()<2) return p;
        p.profile=(PipelineProfile)b[0];
        size_t n=b[1];
        for(size_t i=0;i<n&&2+i<b.size();++i)
            p.encode_order.push_back((LayerID)b[2+i]);
        if(b.size()>=2+n+8)
            for(int i=0;i<8;++i)
                p.profile_seed|=(uint64_t)b[2+n+i]<<(i*8);
        return p;
    }

    // レイヤーが含まれているか
    bool has(LayerID l) const {
        for(auto x:encode_order) if(x==l) return true;
        return false;
    }
};

// ============================================================
// PipelineExecutor — パイプライン実行エンジン
// 実際のデータ変換はここが担当
// ============================================================
struct PipelineContext {
    CPBConfig        config;
    LayerPipeline    pipeline;
    // 変換前後のサイズ追跡
    struct StageResult {
        LayerID  layer;
        size_t   size_before;
        size_t   size_after;
        double   ratio() const {
            return size_before ? (double)size_after/size_before : 1.0;
        }
    };
    std::vector<StageResult> stage_log;
};

// パイプライン実行結果
struct PipelineResult {
    std::vector<uint8_t>        data;      // 変換後データ
    PipelineContext             ctx;
    bool                        success = false;
    std::string                 error;

    // 圧縮効率サマリー
    void print_summary() const;
};

// エンコードパイプライン実行 (シミュレーション)
PipelineResult run_pipeline_encode(
    const std::vector<uint8_t>& input,
    const LayerPipeline& pipeline,
    const CPBConfig& config);

// デコードパイプライン実行 (シミュレーション)
PipelineResult run_pipeline_decode(
    const std::vector<uint8_t>& input,
    const LayerPipeline& pipeline,
    const CPBConfig& config);

// L5_LEARN キャッシュ永続化用エントリ型
struct DslCacheEntry {
    uint64_t             hash;
    std::vector<uint8_t> dsl;
    size_t               orig_size;
};

// L5_LEARN キャッシュ管理
void   l5_learn_reset_cache();   // セッションキャッシュをリセット
size_t l5_learn_cache_size();    // キャッシュエントリ数

// L5_LEARN キャッシュ永続化
std::vector<DslCacheEntry> l5_cache_export();           // キャッシュをエクスポート
void                       l5_cache_import(             // キャッシュをインポート (追加)
    const std::vector<DslCacheEntry>& entries);

// テスト用ラッパー (apply_encode/decode の内部関数を外部公開)
std::vector<uint8_t> apply_encode_test(
    const std::vector<uint8_t>& data, const CPBConfig& cfg);
std::vector<uint8_t> apply_decode_test(
    const std::vector<uint8_t>& data, const CPBConfig& cfg);
