#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

// ============================================================
// CPB Level 1 — フレームインデックス & 検索
//
// Frame 0 に格納する構造:
//   [RVAC_Header]
//   [FrameDirectory: フレームID→オフセット/サイズ]
//   [TrigramIndex:   3バイトNgram→(frame_id,offset)リスト]
//   [LabelIndex:     ラベル文字列→frame_id リスト]
// ============================================================

// ── フレームディレクトリエントリ ──
struct FrameEntry {
    uint32_t frame_id   = 0;
    uint64_t data_offset= 0; // 元データ内のバイトオフセット
    uint32_t data_size  = 0; // このフレームのバイト数
    uint32_t hash32     = 0; // Adler-32 (高速整合性チェック)
    uint8_t  label[32]  = {}; // 任意ラベル (公文書名など)
};

// ── 検索ヒット ──
struct SearchHit {
    uint32_t frame_id   = 0;
    uint64_t byte_offset= 0; // フレーム内オフセット
    uint32_t match_len  = 0;
};

// ── フレームインデックス全体 ──
struct FrameIndex {
    uint32_t version       = 1;
    uint32_t frame_count   = 0;
    uint64_t total_bytes   = 0;
    double   fps           = 30.0; // フレームレート (時間軸マッピング用)

    std::vector<FrameEntry> directory;

    // trigram → (frame_id, offset) のポスティングリスト
    // key = 3バイトを uint32_t に詰めたもの
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t,uint32_t>>> trigram;

    // ラベル → frame_id リスト
    std::unordered_map<std::string, std::vector<uint32_t>> labels;
};

// ── API ──

// インデックスをシリアライズ → バイト列 (Frame0 に格納)
std::vector<uint8_t> serialize_index(const FrameIndex& idx);

// バイト列からインデックスを復元
FrameIndex deserialize_index(const std::vector<uint8_t>& buf);

// Blobリストからインデックスを構築
// labels: frame_id → ラベル文字列 (省略可)
FrameIndex build_index(
    const std::vector<std::vector<uint8_t>>& blobs,
    double fps = 30.0,
    const std::vector<std::string>& labels = {});

// バイトパターン検索 (Boyer-Moore-Horspool)
std::vector<SearchHit> search_pattern(
    const FrameIndex& idx,
    const std::vector<std::vector<uint8_t>>& blobs,
    const std::vector<uint8_t>& pattern);

// 文字列検索 (テキストデータ向け)
std::vector<SearchHit> search_text(
    const FrameIndex& idx,
    const std::vector<std::vector<uint8_t>>& blobs,
    const std::string& query);

// ラベル検索
std::vector<uint32_t> search_label(
    const FrameIndex& idx,
    const std::string& label);

// フレームID範囲取得
std::vector<uint32_t> range_query(
    const FrameIndex& idx,
    double time_start_sec,
    double time_end_sec);

// 時刻 → フレームID
uint32_t time_to_frame(const FrameIndex& idx, double sec);

// フレームID → 時刻
double frame_to_time(const FrameIndex& idx, uint32_t frame_id);
