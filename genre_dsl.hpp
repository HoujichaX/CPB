#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "container.hpp"

// ============================================================
// CPB Level 3 — ジャンル別 DSL 定義
//
// 三層構造:
//   Base DSL    : Level 2 と共通 (RAW/COPY/FILL/END + 新規共通命令)
//   Genre DSL   : ジャンル特有命令
//   Genre Meta  : 意味情報・復元補助
// ============================================================

// ============================================================
// ジャンル ID
// ============================================================
enum class Genre : uint8_t {
    UNKNOWN   = 0x00,
    DOCUMENT  = 0x01,   // 文書系
    IMAGE     = 0x02,   // 画像系
    VIDEO     = 0x03,   // 動画系
    AUDIO     = 0x04,   // 音声系
    ASSET     = 0x05,   // アセット系
    DATABASE  = 0x06,   // データベース系
    MIXED     = 0x07,   // 混合系
};

inline const char* genre_name(Genre g) {
    switch(g) {
    case Genre::DOCUMENT: return "document";
    case Genre::IMAGE:    return "image";
    case Genre::VIDEO:    return "video";
    case Genre::AUDIO:    return "audio";
    case Genre::ASSET:    return "asset";
    case Genre::DATABASE: return "database";
    case Genre::MIXED:    return "mixed";
    default:              return "unknown";
    }
}

// ============================================================
// Base DSL 拡張命令 (Level 2 の OUTPUT/RAW/COPY/FILL/END に追加)
// ============================================================
namespace BaseOp {
    constexpr uint8_t OUTPUT       = 0x01;
    constexpr uint8_t RAW          = 0x02;
    constexpr uint8_t COPY         = 0x03;
    constexpr uint8_t FILL         = 0x04;
    // --- Level 3 追加 ---
    constexpr uint8_t REF_BLOB     = 0x05; // [blob_id:4][dst:8][len:4]  別BlobをBlobプールから参照
    constexpr uint8_t REF_ASSET    = 0x06; // [asset_id:4][dst:8][len:4] 共有アセットプール参照
    constexpr uint8_t APPLY_DELTA  = 0x07; // [src:8][delta_blob:4][dst:8][len:4] 差分適用
    constexpr uint8_t CHECKPOINT   = 0x08; // [label:4]  復元チェックポイント(デバッグ・検証用)
    constexpr uint8_t INLINE_DATA   = 0x09; // [dst:8][len:4][data:len] データをDSL内にインライン展開
    constexpr uint8_t GENRE_BEGIN  = 0x10; // [genre:1]  以降はジャンル命令を使用
    constexpr uint8_t GENRE_END    = 0x11; // []         ジャンル命令ブロック終了
    constexpr uint8_t END          = 0xFF;
}

// ============================================================
// 文書系 DSL (0x20〜0x2F)
// ============================================================
namespace DocOp {
    constexpr uint8_t REF_PHRASE        = 0x20; // [phrase_id:2][dst:8]            辞書登録済み定型句を展開
    constexpr uint8_t REF_SCHEMA        = 0x21; // [schema_id:2][dst:8][len:4]     既知スキーマ構造を再利用
    constexpr uint8_t SECTION_TEMPLATE  = 0x22; // [tmpl_id:2][dst:8][param_len:2] 見出し付き段落テンプレ
    constexpr uint8_t PARAGRAPH_DELTA   = 0x23; // [prev_src:8][dst:8][delta:4]    前段落との差分
    constexpr uint8_t INDENT_PATTERN    = 0x24; // [dst:8][len:4][depth:1][ch:1]   インデントパターン圧縮
}

// ============================================================
// 画像系 DSL (0x30〜0x3F)
// ============================================================
namespace ImgOp {
    constexpr uint8_t TILE_REPEAT       = 0x30; // [tile_src:8][tile_w:4][tile_h:4][count_x:4][count_y:4]  (tile_w/h は uint32_t)
    constexpr uint8_t PALETTE_REGION    = 0x31; // [palette_id:2][dst:8][len:4]    パレット塗り
    constexpr uint8_t ALPHA_MASK_REF    = 0x32; // [mask_blob:4][dst:8][len:4]     透過マスク再利用
    constexpr uint8_t MIRROR_REGION     = 0x33; // [src:8][dst:8][len:4][axis:1]   左右/上下反転再利用
    constexpr uint8_t LINE_BLOCK        = 0x34; // [dst:8][len:4][dir:1][color:4]  罫線/輪郭線
    constexpr uint8_t PATCH_REGION      = 0x35; // [dst:8][patch_blob:4][len:4]    部分更新パッチ
}

// ============================================================
// 動画系 DSL (0x40〜0x4F)
// ============================================================
namespace VidOp {
    constexpr uint8_t FRAME_BASE_REF    = 0x40; // [frame_id:4][dst:8][len:4]      基準フレーム参照
    constexpr uint8_t MOTION_PATCH      = 0x41; // [dst:8][patch_blob:4][len:4]    動き差分パッチ
    constexpr uint8_t BACKGROUND_HOLD   = 0x42; // [bg_blob:4][start:8][frames:4]  背景維持
    constexpr uint8_t SCENE_BOUNDARY    = 0x43; // [frame_id:4][label:2]           シーン切替点
    constexpr uint8_t PLANE_COPY        = 0x44; // [plane:1][src_frame:4][dst:8][len:4] 色/α平面コピー
    constexpr uint8_t FRAME_RUN        = 0x45; // [frame_id:4][count:2]            同一フレーム連続
}

// ============================================================
// 音声系 DSL (0x50〜0x5F)
// ============================================================
namespace AudOp {
    constexpr uint8_t SILENCE_RUN       = 0x50; // [dst:8][samples:4][channels:1]  無音区間
    constexpr uint8_t LOOP_REF          = 0x51; // [src:8][dst:8][len:4][times:2]  ループ参照
    constexpr uint8_t PHRASE_SAMPLE_REF = 0x52; // [sample_blob:4][dst:8][len:4]   音声断片参照
    constexpr uint8_t CHANNEL_DERIVE    = 0x53; // [src_ch:1][dst:8][len:4][op:1]  チャンネル導出
    constexpr uint8_t ENVELOPE_PATCH    = 0x54; // [src:8][env_blob:4][dst:8][len:4] 包絡差分
}

// ============================================================
// アセット系 DSL (0x60〜0x6F)
// ============================================================
namespace AssetOp {
    constexpr uint8_t ASSET_BASE_REF    = 0x60; // [asset_id:4][dst:8][len:4]      共通元アセット参照
    constexpr uint8_t VARIANT_DELTA     = 0x61; // [base_id:4][dst:8][delta:4]     色差分/派生版
    constexpr uint8_t RESOLUTION_DERIVE = 0x62; // [base_id:4][dst:8][scale_num:1][scale_den:1] 解像度派生
    constexpr uint8_t ATLAS_REGION_REF  = 0x63; // [atlas_id:4][x:2][y:2][w:2][h:2][dst:8]      アトラス領域参照
    constexpr uint8_t VERSION_CHAIN     = 0x64; // [prev_id:4][dst:8][delta:4]     版の継承
    constexpr uint8_t SHARED_TEXTURE    = 0x65; // [tex_id:4][dst:8][len:4]        共有テクスチャ参照
}

// ============================================================
// データベース系 DSL (0x70〜0x7F)
// ============================================================
namespace DbOp {
    constexpr uint8_t COLUMN_DICT       = 0x70; // [col_id:2][dict_blob:4][dst:8][len:4] 列辞書圧縮
    constexpr uint8_t SPARSE_MAP        = 0x71; // [dst:8][len:4][null_val:1]       疎構造(NULL多発)
    constexpr uint8_t ROW_DELTA         = 0x72; // [prev_row:8][dst:8][delta:4]     前行差分
    constexpr uint8_t PERIODIC_SERIES   = 0x73; // [dst:8][period:4][count:4]       周期データ
    constexpr uint8_t SCHEMA_REF        = 0x74; // [schema_id:2][dst:8][len:4]      テーブル構造参照
    constexpr uint8_t BLOCK_RANGE_REPEAT= 0x75; // [src:8][dst:8][row_len:4][count:4] 行範囲反復
}

// ============================================================
// 混合系 DSL (0x80〜0x8F)
// ============================================================
namespace MixOp {
    constexpr uint8_t MANIFEST_REF      = 0x80; // [manifest_blob:4][dst:8]         構成表参照
    constexpr uint8_t CROSS_FILE_COPY   = 0x81; // [file_id:4][src:8][dst:8][len:4] 別ファイル範囲再利用
    constexpr uint8_t SHARED_BLOB_POOL  = 0x82; // [pool_blob:4][dst:8][len:4]      共有Blobプール
    constexpr uint8_t DEPENDENCY_LINK   = 0x83; // [dep_id:4][dst:8]                依存関係記録
    constexpr uint8_t SNAPSHOT_DELTA    = 0x84; // [base_id:4][dst:8][delta:4]      スナップショット差分
}

// ============================================================
// Genre Metadata 構造体
// ============================================================

// フレーム境界
struct MetaFrameBoundary {
    uint32_t frame_id;
    uint64_t byte_offset;
    uint8_t  flags;        // 0x01=キーフレーム, 0x02=シーン切替
};

// レイヤー情報
struct MetaLayer {
    uint16_t layer_id;
    uint8_t  type;         // 0x01=前景, 0x02=背景, 0x03=マスク, 0x04=音声
    uint64_t blob_ref;
    char     name[32];
};

// バージョン系統
struct MetaVersion {
    uint32_t version_id;
    uint32_t parent_id;    // 0 = ルート
    uint64_t timestamp;
    char     label[16];
};

// セクション情報
struct MetaSection {
    uint32_t section_id;
    uint8_t  genre;        // Genre enum
    uint64_t start_offset;
    uint64_t length;
};

// ジャンルメタデータコンテナ
struct GenreMetadata {
    Genre    genre;
    uint32_t version       = 1;
    std::vector<MetaFrameBoundary> frames;
    std::vector<MetaLayer>         layers;
    std::vector<MetaVersion>       versions;
    std::vector<MetaSection>       sections;
    std::vector<uint8_t>           extra;  // ジャンル固有の追加情報
};

// ============================================================
// ジャンル別 encode/decode インターフェース
// ============================================================
struct GenreCodec {
    Genre genre;

    // データを解析してジャンルDSLを生成
    // blobs: Level 2 で既にBlobに分割されたデータ
    // meta_out: 生成されたメタデータ (out)
    // 返値: ジャンルDSLバイト列
    virtual std::vector<uint8_t> encode(
        const std::vector<Blob>& blobs,
        GenreMetadata& meta_out) = 0;

    // ジャンルDSLを実行してBlobを復元
    virtual std::vector<uint8_t> decode(
        const std::vector<uint8_t>& dsl,
        const std::vector<Blob>& blobs,
        const GenreMetadata& meta) = 0;

    virtual ~GenreCodec() = default;
};

// ============================================================
// ジャンル自動検出
// ============================================================
Genre detect_genre(const std::vector<uint8_t>& data,
                   const std::string& hint = "");

// L3フレーズ辞書パスを設定 (空文字でリセット)
void l3_set_dict_path(const std::string& path);
#include <vector>
#include <cstdint>

// L3辞書を使った直接フレーズ置換
// (DSL VM不要・シンプル・高速)
std::vector<uint8_t> l3_dict_direct_encode(
    const std::vector<uint8_t>& data, const std::string& dict_path);
std::vector<uint8_t> l3_dict_direct_decode(
    const std::vector<uint8_t>& data, const std::string& dict_path);
bool is_l3_dict_format(const std::vector<uint8_t>& data);
