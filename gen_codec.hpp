#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <functional>

// ============================================================
// CPB Generation Codec — 辞書生成復元レイヤー
//
// 送るのは「辞書ID + パラメータ」だけ。
// 受信側は辞書で元データを生成する。
//
// 辞書種別:
//   STRUCT   構造テンプレート (JSON/HTML/CSV の典型構造)
//   MOTION   モーションパターン (走る/歩く/口パク)
//   AUDIO    音声パターン (環境音/楽器/母音遷移)
//   TEXTURE  テクスチャパターン (タイル/グラデーション)
//   DELTA    差分パッチ (前バージョンからの変化のみ)
// ============================================================

enum class GenreCodecId : uint8_t {
    STRUCT   = 0x01,  // 構造テンプレート
    MOTION   = 0x02,  // モーションパターン
    AUDIO    = 0x03,  // 音声パターン
    TEXTURE  = 0x04,  // テクスチャパターン
    DELTA    = 0x05,  // 差分パッチ
    CUSTOM   = 0xFF,  // カスタム辞書
};

// 辞書エントリ
struct GenDictEntry {
    uint32_t    id       = 0;
    std::string name;
    size_t      out_size = 0;  // 生成されるバイト列のサイズ
    // 生成関数: seed + params → bytes
    std::function<std::vector<uint8_t>(uint64_t seed,
                                       const std::vector<uint8_t>& params)> generate;
};

// 生成指示 (これだけ送ればデータが復元できる)
struct GenInstruction {
    GenreCodecId codec_id  = GenreCodecId::CUSTOM;
    uint32_t     dict_id   = 0;
    uint64_t     seed      = 0;
    std::vector<uint8_t> params;  // 辞書依存の追加パラメータ
    size_t       expected_size = 0;

    // シリアライズ (最小: 14バイト〜)
    std::vector<uint8_t> serialize() const;
    static GenInstruction deserialize(const std::vector<uint8_t>& buf);
};

// 辞書レジストリ
class GenDictionary {
public:
    void register_entry(GenreCodecId cid, GenDictEntry entry);
    bool has(GenreCodecId cid, uint32_t dict_id) const;

    // 指示からデータを生成
    std::vector<uint8_t> generate(const GenInstruction& instr) const;

    // データから最適な生成指示を探す (近似マッチ)
    // threshold: 一致率の下限 (0.0〜1.0)
    struct MatchResult {
        bool          found     = false;
        GenInstruction instr;
        double        match_rate = 0.0;
        size_t        saved_bytes= 0;  // 辞書化で削減できるバイト数
    };
    MatchResult find_best(const std::vector<uint8_t>& data,
                          double threshold = 0.80) const;

    // 辞書一覧
    size_t entry_count() const;

private:
    std::unordered_map<uint32_t,GenDictEntry> entries_; // key = cid<<24|dict_id
    static uint32_t make_key(GenreCodecId c, uint32_t id){
        return ((uint32_t)c<<24)|id;
    }
};

// ── 組み込み辞書ファクトリ ──

// 構造テンプレート辞書を生成 (JSON/HTML/CSV)
void register_struct_dict(GenDictionary& dict);

// テクスチャ辞書 (タイル/グラデ/単色/ノイズ)
void register_texture_dict(GenDictionary& dict);

// 音声辞書 (無音/ホワイトノイズ/正弦波/ブラウンノイズ)
void register_audio_dict(GenDictionary& dict);

// 差分パッチ辞書 (前バージョンとの差分だけ持つ)
void register_delta_dict(GenDictionary& dict);
