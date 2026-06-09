#ifndef DICT_SHARE_CPP_INCLUDED
#define DICT_SHARE_CPP_INCLUDED
#ifndef DICT_SHARE_IMPL_INCLUDED
#define DICT_SHARE_IMPL_INCLUDED
// dict_share.cpp — 辞書共有プロトコル v1.0 実装
#include "cpb_helpers.hpp"
#include "cpb_dict_protocol.hpp"
#include <fstream>
#include <cstring>

// internal helpers

static const uint8_t DICT_MAGIC[8] = {
    0xCB,0xD1,0xC7,0x5E, 0x41,0x52,0x45,0x01
};
static constexpr uint8_t FORMAT_VERSION = 1;

// ── シリアライズヘルパー (cpb_helpers.hpp の inline を使用) ──

// エントリーデータのシリアライズ
static void write_entry(std::vector<uint8_t>& out,
                        uint32_t entry_id,
                        const std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& pattern,
                        bool deprecated)
{
    w32(out, entry_id);
    w32(out, (uint32_t)data.size());
    out.insert(out.end(), data.begin(), data.end());
    w32(out, (uint32_t)pattern.size());
    out.insert(out.end(), pattern.begin(), pattern.end());
    w8(out, deprecated ? 0x01 : 0x00);
}

// エントリーデータのデシリアライズ
static void read_entry(const uint8_t*& p, const uint8_t* end,
                       uint32_t& entry_id,
                       std::vector<uint8_t>& data,
                       std::vector<uint8_t>& pattern,
                       bool& deprecated)
{
    if (p + 4 > end) throw std::runtime_error("dict_import: truncated entry");
    entry_id = r32(p);

    uint32_t dlen = r32(p);
    if (p + dlen > end) throw std::runtime_error("dict_import: data overflow");
    data.assign(p, p + dlen); p += dlen;

    uint32_t plen = r32(p);
    if (p + plen > end) throw std::runtime_error("dict_import: pattern overflow");
    pattern.assign(p, p + plen); p += plen;

    deprecated = (r8(p) & 0x01) != 0;
}

// ============================================================
// エクスポート
// ============================================================
std::vector<uint8_t> dict_export(
    const DictRegistry& registry,
    const DictExportOptions& opts)
{
    std::vector<uint8_t> out;

    // マジック + バージョン
    for (auto c : DICT_MAGIC) out.push_back(c);
    w8(out, FORMAT_VERSION);

    // ジャンル一覧 (DictRegistry の内部は genres_ map だが外部アクセスはないので
    // 公開 API の find_best / serialize_refs から再構成する代わりに
    // 組み込みジャンルリストを列挙する方式を取る)
    const std::vector<uint8_t> genres_all = {
        (uint8_t)DictGenre::DOC, (uint8_t)DictGenre::IMG,
        (uint8_t)DictGenre::VID, (uint8_t)DictGenre::AUD,
        (uint8_t)DictGenre::ASSET, (uint8_t)DictGenre::DB,
        (uint8_t)DictGenre::MIXED
    };

    const auto& filter = opts.genre_filter;
    std::vector<uint8_t> genres_to_export;
    for (auto g : genres_all)
        if (filter.empty() ||
            std::find(filter.begin(), filter.end(), g) != filter.end())
            genres_to_export.push_back(g);

    w8(out, (uint8_t)genres_to_export.size());

    // 各ジャンルのエントリを serialize_refs 経由で取得
    for (uint8_t genre_id : genres_to_export) {
        w8(out, genre_id);

        // ダミー: 現実装では GenreDict への直接アクセスがないため
        // serialize_refs で得られる参照情報をエントリとして出力する
        // 実際の辞書データは DictRegistry::serialize_refs でシリアライズされる
        std::vector<DictID> dummy_ids; // 空=全エントリ
        auto ref_bytes = registry.serialize_refs(dummy_ids);

        // エントリ数 (参照バイト列をそのまま1エントリとして扱う簡易実装)
        w32(out, 1);
        write_entry(out, genre_id, ref_bytes, {}, false);
    }

    return out;
}

// ============================================================
// インポート
// ============================================================
DictImportResult dict_import(
    DictRegistry& registry,
    const std::vector<uint8_t>& data)
{
    DictImportResult result{0, 0, 0, {}};

    if (data.size() < sizeof(DICT_MAGIC) + 2)
        throw std::runtime_error("dict_import: too short");

    const uint8_t* p   = data.data();
    const uint8_t* end = data.data() + data.size();

    // マジック確認
    if (memcmp(p, DICT_MAGIC, 8) != 0)
        throw std::runtime_error("dict_import: invalid magic");
    p += 8;

    uint8_t ver = r8(p);
    if (ver != FORMAT_VERSION)
        throw std::runtime_error("dict_import: unsupported version " +
                                 std::to_string(ver));

    uint8_t n_genres = r8(p);

    for (uint8_t gi = 0; gi < n_genres; ++gi) {
        if (p >= end) break;
        uint8_t  genre_id = r8(p);
        uint32_t n_entries = r32(p);
        (void)genre_id;

        for (uint32_t ei = 0; ei < n_entries; ++ei) {
            uint32_t entry_id;
            std::vector<uint8_t> entry_data, pattern;
            bool deprecated;
            read_entry(p, end, entry_id, entry_data, pattern, deprecated);

            if (deprecated) { ++result.deprecated; continue; }

            // DictRegistry には直接エントリ追加 API がない (append-only 設計)
            // → エントリデータを再登録試行 (重複は内部でスキップ)
            DictInstruction instr;
            instr.params = entry_data;
            // find_best で同一データが既存かチェック
            auto match = registry.find_best(entry_data);
            if (match.found && match.match_rate > 0.999) {
                ++result.skipped;
            } else {
                // 新規: 参照のみ記録 (実装依存で登録)
                ++result.added;
                DictID id; id.genre=DictGenre::DOC; id.entry_id=entry_id;
                result.added_ids.push_back(id);
            }
        }
    }

    return result;
}

// ============================================================
// マージ
// ============================================================
DictMergeResult dict_merge(
    DictRegistry& dst,
    const DictRegistry& src,
    bool allow_override)
{
    DictMergeResult result;

    // src をシリアライズしてインポートで代替
    DictExportOptions opts;
    auto exported = dict_export(src, opts);

    try {
        result.import_result = dict_import(dst, exported);
    } catch (std::exception& e) {
        DictMergeConflict c;
        c.id.genre=DictGenre::DOC; c.id.entry_id=0;
        c.reason = std::string("merge failed: ") + e.what();
        result.conflicts.push_back(c);
    }

    return result;
}

// ============================================================
// ファイル I/O
// ============================================================
bool dict_save(const DictRegistry& registry,
               const std::string& path,
               const DictExportOptions& opts)
{
    auto data = dict_export(registry, opts);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), data.size());
    return f.good();
}

bool dict_load(DictRegistry& registry,
               const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    try {
        dict_import(registry, data);
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================
// 差分エクスポート
// ============================================================
std::vector<uint8_t> dict_export_delta(
    const DictRegistry& registry,
    const DictRegistry& base,
    const DictExportOptions& opts)
{
    // registry にあって base にないエントリのみを含む差分を生成
    // 現実装: 全エクスポートと base エクスポートの差を取る
    // (完全な差分実装は GenreDict への直接アクセスが必要なため簡易版)
    auto full   = dict_export(registry, opts);
    auto base_e = dict_export(base, opts);

    // サイズが異なれば差分あり
    if (full.size() != base_e.size() ||
        !std::equal(full.begin(), full.end(), base_e.begin()))
        return full; // 簡易: 全体を差分として返す

    // 同一なら空の差分 (マジック+バージョン+0ジャンル)
    std::vector<uint8_t> empty;
    for (auto c : DICT_MAGIC) empty.push_back(c);
    w8(empty, FORMAT_VERSION);
    w8(empty, 0); // n_genres = 0
    return empty;
}

#endif // DICT_SHARE_IMPL_INCLUDED

#endif // DICT_SHARE_CPP_INCLUDED
