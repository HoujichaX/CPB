#pragma once
#include "cpb_dict_protocol.hpp"
#include <chrono>
#include <functional>

// ============================================================
// CPB Dictionary Evolution Protocol
// 観測 → 提案 → 承認 の3段階更新フロー
// ============================================================

// ── 観測結果 ──
enum class PatternType : uint8_t {
    MONO_COLOR,       // 単色・単一バイト繰り返し
    TILE_REPEAT,      // 周期的タイルパターン
    GRADIENT,         // 単調増加/減少
    SILENCE,          // 無音(ほぼゼロ)
    SINE_LIKE,        // 周期的波形
    FRAME_RUN,        // フレーム反復
    SPARSE_CSV,       // NULL/カンマ多発
    JSON_STRUCT,      // JSON配列/オブジェクト構造
    CSV_TABLE,        // CSV表形式
    LOG_FORMAT,       // タイムスタンプ+レベルのログ
    UNKNOWN,          // 分類不能
};

inline const char* pattern_name(PatternType p) {
    switch(p){
    case PatternType::MONO_COLOR:  return "MONO_COLOR";
    case PatternType::TILE_REPEAT: return "TILE_REPEAT";
    case PatternType::GRADIENT:    return "GRADIENT";
    case PatternType::SILENCE:     return "SILENCE";
    case PatternType::SINE_LIKE:   return "SINE_LIKE";
    case PatternType::FRAME_RUN:   return "FRAME_RUN";
    case PatternType::SPARSE_CSV:  return "SPARSE_CSV";
    case PatternType::JSON_STRUCT: return "JSON_STRUCT";
    case PatternType::CSV_TABLE:   return "CSV_TABLE";
    case PatternType::LOG_FORMAT:  return "LOG_FORMAT";
    default:                       return "UNKNOWN";
    }
}

struct Observation {
    PatternType type     = PatternType::UNKNOWN;
    DictGenre   genre    = DictGenre::DOC;
    double      strength = 0.0;   // パターンの強さ [0.0, 1.0]
    size_t      data_size= 0;
    std::vector<uint8_t> params;  // パターン記述パラメータ
    std::string detail;           // 人間向け説明
};

// ── 提案 (Proposal) ──
enum class ProposalStatus : uint8_t {
    PENDING,   // 審査待ち
    APPROVED,  // 承認済み → GenreDict に追加
    REJECTED,  // 却下
};

struct Proposal {
    uint32_t     id        = 0;
    Observation  obs;
    ProposalStatus status  = ProposalStatus::PENDING;
    double       match_rate= 0.0;
    size_t       bytes_saved=0;
    std::string  suggested_name;
    std::string  description;
    uint64_t     timestamp = 0;

    // 承認されたときに追加されるエントリ情報
    DictGenre    target_genre;
    uint32_t     assigned_entry_id = 0;

    std::string status_str() const {
        switch(status){
        case ProposalStatus::PENDING:  return "PENDING";
        case ProposalStatus::APPROVED: return "APPROVED";
        case ProposalStatus::REJECTED: return "REJECTED";
        }
        return "?";
    }
};

// ── 3段階プロトコル ──
class DictEvolution {
public:
    explicit DictEvolution(DictRegistry& registry)
        : registry_(registry) {}

    // ============================================================
    // Step 1: 観測 — データからパターンを検出
    // ============================================================
    Observation observe(const std::vector<uint8_t>& data,
                        DictGenre hint = DictGenre::MIXED) const;

    // 複数パターンを検出 (強い順)
    std::vector<Observation> observe_all(
        const std::vector<uint8_t>& data) const;

    // ============================================================
    // Step 2: 提案 — 既存辞書にないパターンなら提案を作成
    // ============================================================
    // 観測結果を提案に変換 (既存辞書で十分カバーできる場合はnullopt)
    std::optional<Proposal> propose(const Observation& obs,
                                    double novelty_threshold = 0.70);

    // データを直接渡して観測+提案を一括実行
    std::optional<Proposal> observe_and_propose(
        const std::vector<uint8_t>& data,
        DictGenre hint = DictGenre::MIXED);

    // 提案をストアに追加
    uint32_t submit(Proposal p);

    // ============================================================
    // Step 3: 承認 — 人間が判断してGenreDictに追加
    // ============================================================
    bool approve(uint32_t proposal_id,
                 const std::string& name = "",
                 const std::string& description = "");

    bool reject(uint32_t proposal_id);

    // ============================================================
    // 提案ストア操作
    // ============================================================
    std::vector<const Proposal*> pending() const;
    std::vector<const Proposal*> all()     const;
    const Proposal* get(uint32_t id)       const;
    size_t pending_count()                 const;

    // シリアライズ (提案をファイル保存)
    std::vector<uint8_t> serialize_proposals() const;
    void load_proposals(const std::vector<uint8_t>& buf);

private:
    DictRegistry&            registry_;
    std::vector<Proposal>    proposals_;
    uint32_t                 next_id_   = 1;
    uint32_t                 next_entry_= 0x9000; // ユーザー追加エントリは0x9000〜

    // パターン検出実装
    Observation detect_mono   (const std::vector<uint8_t>& d) const;
    Observation detect_tile   (const std::vector<uint8_t>& d) const;
    Observation detect_gradient(const std::vector<uint8_t>&d) const;
    Observation detect_silence(const std::vector<uint8_t>& d) const;
    Observation detect_sine   (const std::vector<uint8_t>& d) const;
    Observation detect_frame_run(const std::vector<uint8_t>&d) const;
    Observation detect_doc    (const std::vector<uint8_t>& d) const;

    // 提案からDictEntryを構築
    DictEntry build_entry(const Proposal& p) const;
};

// ── Layer5コーデック ──
// GenreDictを使ってエンコード/デコードを行い、
// マッチしなかった場合はDictEvolutionに観測を提出する
struct Layer5Result {
    bool    used_dict   = false;    // 辞書を使えたか
    double  match_rate  = 0.0;      // 辞書一致率
    size_t  orig_size   = 0;
    size_t  encoded_size= 0;        // 指示サイズ (辞書使用時) or 元サイズ

    // 辞書使用時: 生成指示
    std::optional<DictInstruction> instruction;
    // 辞書未使用時: 新規提案ID (観測が提出された場合)
    std::optional<uint32_t> proposal_id;
};

class Layer5Codec {
public:
    Layer5Codec(DictRegistry& reg, DictEvolution& evo)
        : registry_(reg), evolution_(evo) {}

    // エンコード: 辞書にマッチすれば指示を返す、しなければ観測提出
    Layer5Result encode(const std::vector<uint8_t>& data,
                        DictGenre hint,
                        double threshold = 0.80);

    // デコード: 指示からデータを生成
    std::vector<uint8_t> decode(const DictInstruction& instr);

    // 統計
    struct Stats {
        size_t encode_count   = 0;
        size_t dict_hit       = 0;
        size_t dict_miss      = 0;
        size_t proposals_made = 0;
        double avg_match_rate = 0.0;
        size_t total_bytes_in = 0;
        size_t total_bytes_out= 0;
    };
    Stats stats() const { return stats_; }

private:
    DictRegistry&  registry_;
    DictEvolution& evolution_;
    Stats          stats_;
};
