#pragma once
#include "frame_index.hpp"
#include <string>
#include <vector>
#include <functional>
#include <variant>
#include <optional>

// ============================================================
// CPB Level1 外部検索API
//
// 公開するもの: FIDXのメタデータのみ
// 公開しないもの: Blobのバイト列、Level2/3/4の内容
//
// クエリ種別:
//   PATTERN  バイト列パターン (BMH)
//   TEXT     テキスト文字列
//   LABEL    ラベルタグ
//   TIME     時間範囲 [start_sec, end_sec]
//   HASH32   Adler-32 で特定フレームを直接指定
//   AND/OR   クエリ合成
// ============================================================

// ── クエリ ──
enum class QueryType {
    PATTERN, TEXT, LABEL, TIME_RANGE, HASH32, AND, OR
};

struct Query {
    QueryType type;

    // PATTERN / TEXT
    std::vector<uint8_t> pattern;
    std::string          text;

    // LABEL
    std::string label;

    // TIME_RANGE
    double time_start = 0.0;
    double time_end   = 0.0;

    // HASH32
    uint32_t hash32 = 0;

    // AND / OR (複合クエリ)
    std::vector<Query> children;

    // ファクトリ
    static Query pattern_query(const std::vector<uint8_t>& p){
        Query q; q.type=QueryType::PATTERN; q.pattern=p; return q;
    }
    static Query text_query(const std::string& t){
        Query q; q.type=QueryType::TEXT; q.text=t; return q;
    }
    static Query label_query(const std::string& l){
        Query q; q.type=QueryType::LABEL; q.label=l; return q;
    }
    static Query time_query(double s, double e){
        Query q; q.type=QueryType::TIME_RANGE;
        q.time_start=s; q.time_end=e; return q;
    }
    static Query hash_query(uint32_t h){
        Query q; q.type=QueryType::HASH32; q.hash32=h; return q;
    }
    static Query and_query(std::vector<Query> c){
        Query q; q.type=QueryType::AND; q.children=std::move(c); return q;
    }
    static Query or_query(std::vector<Query> c){
        Query q; q.type=QueryType::OR; q.children=std::move(c); return q;
    }
};

// ── 検索結果 (メタデータのみ、本体データなし) ──
struct SearchResult {
    uint32_t    frame_id    = 0;
    uint64_t    byte_offset = 0;   // 全体バイト列でのオフセット
    uint32_t    match_len   = 0;
    std::string label;             // フレームのラベル (あれば)
    double      timestamp   = 0.0; // 秒
    uint32_t    hash32      = 0;   // フレームのAdler-32
    QueryType   matched_by;        // どのクエリにヒットしたか
};

// ── 検索レスポンス ──
struct SearchResponse {
    std::vector<SearchResult> hits;
    uint32_t  total_frames   = 0;
    uint32_t  scanned_frames = 0;
    double    elapsed_ms     = 0.0;
    std::string query_summary;
};

// ── 検索エンジン ──
class SearchEngine {
public:
    // インデックスをロード (Blobは持たない — メタのみ)
    void load_index(const FrameIndex& idx);

    // フルテキスト検索用にBlobを登録 (オプション)
    // 登録しない場合はラベル/時間/hashのみ検索可能
    void load_blobs(const std::vector<std::vector<uint8_t>>& blobs);

    // 検索実行
    SearchResponse search(const Query& q) const;

    // インデックス情報 (公開メタのみ)
    struct IndexInfo {
        uint32_t frame_count;
        uint64_t total_bytes;
        double   fps;
        double   duration_sec;
        size_t   trigram_entries;
        size_t   label_count;
        size_t   index_size_bytes;
    };
    IndexInfo info() const;

    // シリアライズ (FIDXをバイト列に) — Blob本体は含まない
    std::vector<uint8_t> export_index() const;
    void import_index(const std::vector<uint8_t>& buf);

private:
    FrameIndex                              idx_;
    std::vector<std::vector<uint8_t>>       blobs_; // オプション
    bool                                    has_blobs_ = false;

    SearchResponse search_pattern_impl(const Query& q) const;
    SearchResponse search_label_impl(const Query& q) const;
    SearchResponse search_time_impl(const Query& q) const;
    SearchResponse search_hash_impl(const Query& q) const;
    SearchResponse merge_and(const std::vector<SearchResponse>& rs) const;
    SearchResponse merge_or (const std::vector<SearchResponse>& rs) const;
};

// ── クエリ → 人間向け文字列 ──
std::string query_to_string(const Query& q);
