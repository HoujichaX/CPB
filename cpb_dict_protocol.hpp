#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <optional>

// ============================================================
// CPB Dictionary Protocol v1.0
//
// 辞書は「ジャンル別DSLと同じ分類」で分割管理する。
// 総合辞書は作らない。必要な辞書だけロード。
//
// 辞書ID体系:
//   [genre:8bit][major:8bit][minor:8bit][subtag:8bit][entry:32bit]
//   = 64bit で一意
//
// バージョン体系:
//   major.minor[subtag]   例: 1.2a, 2.0, 1.3b
//   major: 社会構造の変化 (1=Web文化, 2=AI文化, 3=生成文化...)
//   minor: 辞書の大規模追加
//   subtag: 非推奨化・整理 (a,b,c... → 0=なし)
//
// 更新プロトコル:
//   ADD:        新エントリを追加 (既存は触らない)
//   DEPRECATE:  非推奨フラグを立てる (削除しない)
//   REBASE:     メジャー番号を上げる (旧辞書はそのまま残す)
// ============================================================

// ── ジャンルID (Genre DSLと1:1対応) ──
enum class DictGenre : uint8_t {
    DOC    = 0x01,  // 文書 (JSON/HTML/CSV/Markdown)
    IMG    = 0x02,  // 画像 (タイル/パレット/エッジ)
    VID    = 0x03,  // 動画 (モーション/カメラ/背景差分)
    AUD    = 0x04,  // 音声 (母音/楽器ADSR/環境音)
    ASSET  = 0x05,  // アセット (3D差分/テクスチャ/UIパーツ)
    DB     = 0x06,  // データベース (行差分/時系列/スパース)
    MIXED  = 0x07,  // 複合 (ゲームシーンなど複数ジャンル跨ぎ)
};

inline const char* genre_name(DictGenre g) {
    switch(g){
    case DictGenre::DOC:   return "DocDict";
    case DictGenre::IMG:   return "ImgDict";
    case DictGenre::VID:   return "VidDict";
    case DictGenre::AUD:   return "AudDict";
    case DictGenre::ASSET: return "AssetDict";
    case DictGenre::DB:    return "DBDict";
    case DictGenre::MIXED: return "MixedDict";
    default:               return "UnknownDict";
    }
}

// ── バージョン ──
struct DictVersion {
    uint8_t major  = 1;
    uint8_t minor  = 0;
    uint8_t subtag = 0;  // 0=なし, 1='a', 2='b', ...

    bool operator==(const DictVersion& o) const {
        return major==o.major && minor==o.minor && subtag==o.subtag;
    }
    bool operator<(const DictVersion& o) const {
        if(major!=o.major) return major<o.major;
        if(minor!=o.minor) return minor<o.minor;
        return subtag<o.subtag;
    }
    std::string str() const {
        std::string s = std::to_string(major)+"."+std::to_string(minor);
        if(subtag) s += (char)('a'+subtag-1);
        return s;
    }
    static DictVersion parse(const std::string& s) {
        DictVersion v;
        size_t dot = s.find('.');
        if(dot==std::string::npos) { v.major=(uint8_t)std::stoi(s); return v; }
        v.major=(uint8_t)std::stoi(s.substr(0,dot));
        std::string rest=s.substr(dot+1);
        if(!rest.empty() && std::isalpha(rest.back())){
            v.subtag=(uint8_t)(rest.back()-'a'+1);
            rest.pop_back();
        }
        v.minor=(uint8_t)std::stoi(rest);
        return v;
    }
};

// ── 辞書ID (64bit) ──
struct DictID {
    DictGenre   genre;
    DictVersion version;
    uint32_t    entry_id;

    uint64_t encode() const {
        return ((uint64_t)(uint8_t)genre  << 56)
             | ((uint64_t)version.major   << 48)
             | ((uint64_t)version.minor   << 40)
             | ((uint64_t)version.subtag  << 32)
             | (uint64_t)entry_id;
    }
    static DictID decode(uint64_t v) {
        DictID d;
        d.genre          = (DictGenre)((v>>56)&0xFF);
        d.version.major  = (v>>48)&0xFF;
        d.version.minor  = (v>>40)&0xFF;
        d.version.subtag = (v>>32)&0xFF;
        d.entry_id       = v&0xFFFFFFFF;
        return d;
    }
    std::string str() const {
        return std::string(genre_name(genre))
             + " v" + version.str()
             + " #" + std::to_string(entry_id);
    }
};

// ── 辞書エントリ ──
struct DictEntry {
    DictID      id;
    std::string name;
    std::string description;
    bool        deprecated = false;
    DictVersion added_in;      // 追加されたバージョン
    DictVersion deprecated_in; // 非推奨になったバージョン

    // 生成関数: (seed, params) → bytes
    std::function<std::vector<uint8_t>(uint64_t, const std::vector<uint8_t>&)> generate;

    // 近似マッチ関数: data → 一致率 [0.0, 1.0]
    std::function<double(const std::vector<uint8_t>&)> match;
};

// ── 生成指示 (送信側が送るもの) ──
struct DictInstruction {
    DictID               dict_id;
    uint64_t             seed     = 0;
    std::vector<uint8_t> params;
    size_t               expected_size = 0;

    std::vector<uint8_t> serialize() const;
    static DictInstruction deserialize(const std::vector<uint8_t>& buf);

    // 指示のサイズ (バイト)
    size_t instruction_size() const { return 8+8+4+params.size(); }
};

// ── ジャンル別辞書 ──
class GenreDict {
public:
    explicit GenreDict(DictGenre genre) : genre_(genre) {}

    DictGenre genre() const { return genre_; }

    // append-only: 既存エントリは触らない
    void add_entry(DictEntry entry);

    // 非推奨フラグ (削除はしない)
    void deprecate(uint32_t entry_id, DictVersion in_version);

    // エントリ取得
    const DictEntry* get(uint32_t entry_id) const;
    const DictEntry* get_latest(uint32_t entry_id) const;

    // 生成
    std::vector<uint8_t> generate(const DictInstruction& instr) const;

    // 近似マッチ
    struct MatchResult {
        bool           found     = false;
        DictInstruction instr;
        double         match_rate = 0.0;
        size_t         bytes_saved= 0;
    };
    MatchResult find_best(const std::vector<uint8_t>& data,
                          double threshold = 0.80) const;

    // 統計
    size_t entry_count()      const { return entries_.size(); }
    size_t active_count()     const;
    size_t deprecated_count() const;
    DictVersion current_version() const { return current_ver_; }

    // シリアライズ (辞書メタデータのみ, generate関数は含まない)
    std::vector<uint8_t> serialize_meta() const;

private:
    DictGenre   genre_;
    DictVersion current_ver_{1,0,0};
    std::unordered_map<uint32_t, std::vector<DictEntry>> entries_; // entry_id → 履歴
};

// ── 辞書レジストリ (全ジャンルを管理) ──
class DictRegistry {
public:
    // ジャンル辞書を登録
    void register_genre(DictGenre genre);

    // ジャンル辞書を取得
    GenreDict* get_genre(DictGenre genre);
    const GenreDict* get_genre(DictGenre genre) const;

    // 辞書IDから直接取得
    const DictEntry* get_entry(const DictID& id) const;

    // 生成
    std::vector<uint8_t> generate(const DictInstruction& instr) const;

    // 全ジャンルで近似マッチ
    GenreDict::MatchResult find_best(const std::vector<uint8_t>& data,
                                     DictGenre hint = DictGenre::DOC,
                                     double threshold = 0.80) const;

    // 統計
    void print_stats() const;

    // 辞書メタをシリアライズ (CPBヘッダーに埋め込む辞書参照情報)
    std::vector<uint8_t> serialize_refs(
        const std::vector<DictID>& used_ids) const;

private:
    std::unordered_map<uint8_t, GenreDict> genres_;
};

// ── 組み込み辞書ファクトリ ──
void register_doc_dict  (GenreDict& d, const DictVersion& ver = {1,0,0});
void register_img_dict  (GenreDict& d, const DictVersion& ver = {1,0,0});
void register_vid_dict  (GenreDict& d, const DictVersion& ver = {1,0,0});
void register_aud_dict  (GenreDict& d, const DictVersion& ver = {1,0,0});
void register_asset_dict(GenreDict& d, const DictVersion& ver = {1,0,0});
void register_db_dict   (GenreDict& d, const DictVersion& ver = {1,0,0});

// 全辞書を一括登録
DictRegistry make_default_registry();
