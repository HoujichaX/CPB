#ifndef GENRE_DSL_VM_CPP_INCLUDED
#define GENRE_DSL_VM_CPP_INCLUDED
// genre_dsl_vm.cpp
// CPB Level 3 — ジャンル対応拡張 VM
//
// Level 2 の run_vm を拡張し、Base DSL 拡張命令と
// ジャンル命令のディスパッチを担う。
// 各ジャンル命令の本体は genre_handlers/ 以下に分離予定。

#include "genre_dsl.hpp"
#include "dsl_vm.hpp"
#include <cstring>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <string>

// ============================================================
// アセットプール (REF_ASSET 用)
// pack 時に登録、unpack 時に参照
// ============================================================
struct AssetPool {
    std::vector<std::vector<uint8_t>> assets;

    uint32_t add(std::vector<uint8_t> data) {
        uint32_t id = static_cast<uint32_t>(assets.size());
        assets.push_back(std::move(data));
        return id;
    }

    const std::vector<uint8_t>& get(uint32_t id) const {
        if (id >= assets.size())
            throw std::runtime_error("AssetPool: invalid id");
        return assets[id];
    }
};

// ============================================================
// ジャンル VM 実行コンテキスト
// ============================================================
// PhraseDict (REF_PHRASEのデコード用 — genre_dsl.cpp と同じ実装)
struct VmPhraseDict {
    static constexpr size_t MIN_PHRASE = 8;
    std::vector<std::vector<uint8_t>> phrases;
    std::unordered_map<std::string,uint16_t> index;
    void build(const std::vector<uint8_t>& data) {
        if(data.size() < MIN_PHRASE*3) return;
        std::unordered_map<std::string,int> freq;
        for(size_t i=0;i+MIN_PHRASE<=data.size();++i) {
            std::string ng(data.begin()+i,data.begin()+i+MIN_PHRASE);
            freq[ng]++;
        }
        std::vector<std::pair<int,std::string>> sorted;
        for(auto& [k,v]:freq) if(v>=3) sorted.push_back({v,k});
        std::sort(sorted.rbegin(),sorted.rend());
        for(auto& [cnt,ng]:sorted) {
            if(phrases.size()>=256) break;
            uint16_t id=(uint16_t)phrases.size();
            phrases.push_back({ng.begin(),ng.end()});
            index[ng]=id;
        }
    }
};

struct GenreVmContext {
    const std::vector<Blob>&   blobs;
    const GenreMetadata&       meta;
    AssetPool                  pool;
    Genre                      current_genre = Genre::UNKNOWN;
    mutable VmPhraseDict       phrase_dict;  // REF_PHRASE用辞書キャッシュ
    mutable bool               phrase_dict_built = false;
    uint64_t                   dst_cursor    = 0; // 次に書き込む出力位置
};

// ============================================================
// 読み取りヘルパー
// ============================================================
static uint8_t  r8 (const std::vector<uint8_t>& c, size_t& pc){ return c[pc++]; }
static uint16_t r16(const std::vector<uint8_t>& c, size_t& pc){
    uint16_t v; memcpy(&v,&c[pc],2); pc+=2; return v; }
static uint32_t r32(const std::vector<uint8_t>& c, size_t& pc){
    uint32_t v; memcpy(&v,&c[pc],4); pc+=4; return v; }
static uint64_t r64(const std::vector<uint8_t>& c, size_t& pc){
    uint64_t v; memcpy(&v,&c[pc],8); pc+=8; return v; }

// ============================================================
// ジャンル命令ハンドラ (各ジャンル共通インターフェース)
// 返値: 消費バイト数 (0 = このジャンルでは未実装 → 例外)
// ============================================================

// --- 文書系 (0x20〜) ---
static void handle_doc(uint8_t op, const std::vector<uint8_t>& code,
                        size_t& pc, std::vector<uint8_t>& out,
                        GenreVmContext& ctx)
{
    (void)ctx;
    switch (op) {
    case DocOp::REF_PHRASE: {
        // [phrase_id:2][dst:8][len:4]
        uint16_t phrase_id = r16(code, pc);
        uint64_t dst       = r64(code, pc);
        uint32_t match_len = r32(code, pc); // 実際のマッチ長

        // 初回のみ辞書を構築
        if (!ctx.phrase_dict_built) {
            std::vector<uint8_t> flat;
            for (auto& b : ctx.blobs)
                flat.insert(flat.end(), b.data.begin(), b.data.end());
            ctx.phrase_dict.build(flat);
            ctx.phrase_dict_built = true;
        }

        if (phrase_id < ctx.phrase_dict.phrases.size()) {
            const auto& phrase = ctx.phrase_dict.phrases[phrase_id];
            size_t len = phrase.size();
            if (dst + len <= out.size())
                std::copy(phrase.begin(), phrase.end(), out.data() + dst);
        }
        break;
    }
    case DocOp::REF_SCHEMA: {
        // [schema_id:2][dst:8][len:4]
        // 既知JSONスキーマ構造テンプレートを dst に len バイト展開
        uint16_t schema_id = r16(code, pc);
        uint64_t dst       = r64(code, pc);
        uint32_t len       = r32(code, pc);
        if (dst + len > out.size())
            throw std::runtime_error("DocOp::REF_SCHEMA: out of bounds");
        // meta.extra[0..] にスキーマデータが格納されている場合は使用
        // なければ schema_id を seed としたパターンで埋める
        if (!ctx.meta.extra.empty() &&
            (size_t)schema_id < ctx.meta.extra.size()) {
            uint8_t pattern = ctx.meta.extra[schema_id];
            std::fill(out.data()+dst, out.data()+dst+len, pattern);
        } else {
            // フォールバック: schema_id を XOR 種にしたバイト列
            for (uint32_t i = 0; i < len; ++i)
                out[dst + i] = (uint8_t)((schema_id ^ i) & 0xFF);
        }
        break;
    }
    case DocOp::SECTION_TEMPLATE: {
        // [tmpl_id:2][dst:8][param_len:2]
        // 見出し付き段落テンプレート: tmpl_id が指すテンプレート構造を展開
        uint16_t tmpl_id   = r16(code, pc);
        uint64_t dst       = r64(code, pc);
        uint16_t param_len = r16(code, pc);
        // param_len バイトのパラメータがコード中に続く
        if (pc + param_len > code.size())
            throw std::runtime_error("DocOp::SECTION_TEMPLATE: param out of bounds");
        const uint8_t* params = code.data() + pc;
        pc += param_len;
        // テンプレート: tmpl_id に応じた固定ヘッダー構造を出力領域に適用
        // 現実装: params を dst から書き込む (テンプレートはパラメータ直接展開)
        if (dst + param_len <= out.size())
            std::copy(params, params + param_len, out.data() + dst);
        break;
    }
    case DocOp::PARAGRAPH_DELTA: {
        // [prev_src:8][dst:8][delta:4] 前段落との XOR 差分を適用
        uint64_t prev_src = r64(code, pc);
        uint64_t dst      = r64(code, pc);
        uint32_t dlen     = r32(code, pc);
        if (pc + dlen > code.size())
            throw std::runtime_error("DocOp::PARAGRAPH_DELTA: delta out of bounds");
        if (dst + dlen > out.size() || prev_src + dlen > out.size())
            throw std::runtime_error("DocOp::PARAGRAPH_DELTA: out of bounds");
        // delta バイト列は XOR パッチ: dst[i] = prev[i] XOR delta[i]
        for (uint32_t i = 0; i < dlen; ++i)
            out[dst + i] = out[prev_src + i] ^ code[pc + i];
        pc += dlen;
        break;
    }
    case DocOp::INDENT_PATTERN: {
        // [dst:8][len:4][depth:1][ch:1] — インデント文字 ch を depth 個 len バイト分埋める
        uint64_t dst   = r64(code, pc);
        uint32_t len   = r32(code, pc);
        uint8_t  depth = r8(code,  pc);
        uint8_t  ch    = r8(code,  pc);
        (void)depth;
        if (dst + len > out.size())
            throw std::runtime_error("DocOp::INDENT_PATTERN: out of bounds");
        memset(out.data() + dst, ch, len);
        break;
    }
    default:
        throw std::runtime_error("Unknown DocOp: " + std::to_string(op));
    }
}

// --- 画像系 (0x30〜) ---
static void handle_img(uint8_t op, const std::vector<uint8_t>& code,
                        size_t& pc, std::vector<uint8_t>& out,
                        GenreVmContext& ctx)
{
    switch (op) {
    case ImgOp::TILE_REPEAT: {
        // [tile_src:8][tile_w:2][tile_h:2][dst_x:4][dst_y:4][count_x:2][count_y:2]
        // RAWで先頭タイルが out[tile_src..tile_src+tile_w] に書き込まれた後に呼ばれる
        // 2回目以降のタイルを複製する
        uint64_t tile_src = r64(code,pc);
        uint32_t tile_w   = r32(code,pc);  // uint32_t に変更
        uint32_t tile_h   = r32(code,pc);
        uint32_t count_x  = r32(code,pc);
        uint32_t count_y  = r32(code,pc);
        (void)tile_h; (void)count_y;
        // count_x 回分のタイルを先頭から複製 (1回目はRAW済みなので2回目から)
        for(uint32_t c = 1; c < count_x; ++c) {
            uint64_t wpos = tile_src + (uint64_t)c * tile_w;
            if(wpos + tile_w > out.size()) break;
            memcpy(out.data() + wpos, out.data() + tile_src, tile_w);
        }
        break;
    }
    case ImgOp::PALETTE_REGION: {
        uint16_t pid = r16(code,pc); uint64_t dst = r64(code,pc); uint32_t len = r32(code,pc);
        (void)pid;
        if(dst+len>out.size()) throw std::runtime_error("ImgOp::PALETTE_REGION: out of bounds");
        memset(out.data()+dst, 0, len);
        break;
    }
    case ImgOp::ALPHA_MASK_REF: {
        uint32_t bid=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        const auto& m=ctx.blobs.at(bid).data;
        if(len>m.size()||dst+len>out.size()) throw std::runtime_error("ImgOp::ALPHA_MASK_REF: out of bounds");
        memcpy(out.data()+dst, m.data(), len);
        break;
    }
    case ImgOp::MIRROR_REGION: {
        uint64_t src=r64(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc); uint8_t axis=r8(code,pc);
        (void)axis;
        if(src+len>out.size()||dst+len>out.size()) throw std::runtime_error("ImgOp::MIRROR_REGION: out of bounds");
        for(uint32_t i=0;i<len;++i) out[dst+i]=out[src+len-1-i];
        break;
    }
    case ImgOp::LINE_BLOCK: {
        uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc); uint8_t dir=r8(code,pc);
        uint8_t cr=r8(code,pc),cg=r8(code,pc),cb=r8(code,pc),ca=r8(code,pc);
        (void)dir;(void)cg;(void)cb;(void)ca;
        if(dst+len>out.size()) throw std::runtime_error("ImgOp::LINE_BLOCK: out of bounds");
        memset(out.data()+dst, cr, len);
        break;
    }
    case ImgOp::PATCH_REGION: {
        uint64_t dst=r64(code,pc); uint32_t patch_id=r32(code,pc); uint32_t len=r32(code,pc);
        const auto& p=ctx.blobs.at(patch_id).data;
        if(len>p.size()||dst+len>out.size()) throw std::runtime_error("ImgOp::PATCH_REGION: out of bounds");
        memcpy(out.data()+dst, p.data(), len);
        break;
    }
    default:
        throw std::runtime_error("Unknown ImgOp: " + std::to_string(op));
    }
}

// --- 動画系 (0x40〜) ---
static void handle_vid(uint8_t op, const std::vector<uint8_t>& code,
                        size_t& pc, std::vector<uint8_t>& out,
                        GenreVmContext& ctx)
{
    switch (op) {
    case VidOp::FRAME_BASE_REF: {
        // [frame_id:4][dst:8][len:4] — blobs[frame_id] をそのままコピー (キーフレーム)
        uint32_t fid = r32(code, pc);
        uint64_t dst = r64(code, pc);
        uint32_t len = r32(code, pc);
        const auto& fr = ctx.blobs.at(fid).data;
        if (len > fr.size() || dst + len > out.size())
            throw std::runtime_error("VidOp::FRAME_BASE_REF: out of bounds");
        memcpy(out.data() + dst, fr.data(), len);
        ctx.dst_cursor = dst + len;
        break;
    }
    case VidOp::MOTION_PATCH: {
        // [dst:8][patch_blob:4][len:4]
        // patch_blob は現フレームそのもの (blob index)
        // デコード: out[dst..dst+len] = blobs[patch_blob][0..len]
        uint64_t dst      = r64(code, pc);
        uint32_t patch_id = r32(code, pc);
        uint32_t len      = r32(code, pc);
        const auto& patch = ctx.blobs.at(patch_id).data;
        if (len > patch.size() || dst + len > out.size())
            throw std::runtime_error("VidOp::MOTION_PATCH: out of bounds");
        memcpy(out.data() + dst, patch.data(), len);
        ctx.dst_cursor = dst + len;
        break;
    }
    case VidOp::BACKGROUND_HOLD: {
        // [bg_blob:4][start:8][frames:4] — bg_blob を frames 回コピー
        uint32_t bg_id  = r32(code, pc);
        uint64_t start  = r64(code, pc);
        uint32_t frames = r32(code, pc);
        const auto& bg  = ctx.blobs.at(bg_id).data;
        // start は bg_blob の開始位置、その直後から frames 個コピー
        uint64_t dst = start + bg.size();
        for (uint32_t f = 0; f < frames; ++f) {
            if (dst + bg.size() > out.size()) break;
            memcpy(out.data() + dst, bg.data(), bg.size());
            dst += bg.size();
        }
        ctx.dst_cursor = dst;
        break;
    }
    case VidOp::SCENE_BOUNDARY: {
        // [frame_id:4][label:2] — メタ情報のみ、出力には影響なし
        r32(code, pc); r16(code, pc);
        break;
    }
    case VidOp::PLANE_COPY: {
        // [plane:1][src_frame:4][dst:8][len:4]
        uint8_t  plane    = r8(code,  pc);
        uint32_t src_fid  = r32(code, pc);
        uint64_t dst      = r64(code, pc);
        uint32_t len      = r32(code, pc);
        (void)plane;
        const auto& src_fr = ctx.blobs.at(src_fid).data;
        if (len > src_fr.size() || dst + len > out.size())
            throw std::runtime_error("VidOp::PLANE_COPY: out of bounds");
        memcpy(out.data() + dst, src_fr.data(), len);
        break;
    }
    case VidOp::FRAME_RUN: {
        // [frame_id:4][count:2] — blobs[frame_id] を count 回繰り返す
        // dst_cursor は直前の FRAME_BASE_REF が設定した位置
        uint32_t fid   = r32(code, pc);
        uint16_t count = r16(code, pc);
        const auto& fr = ctx.blobs.at(fid).data;
        uint64_t dst   = ctx.dst_cursor;
        for (uint16_t c = 0; c < count; ++c) {
            if (dst + fr.size() > out.size()) break;
            memcpy(out.data() + dst, fr.data(), fr.size());
            dst += fr.size();
        }
        ctx.dst_cursor = dst;
        break;
    }
    default:
        throw std::runtime_error("Unknown VidOp: " + std::to_string(op));
    }
}

// --- 音声系 (0x50〜) ---
static void handle_aud(uint8_t op, const std::vector<uint8_t>& code,
                        size_t& pc, std::vector<uint8_t>& out,
                        GenreVmContext& ctx)
{
    switch (op) {
    case AudOp::SILENCE_RUN: {
        uint64_t dst=r64(code,pc); uint32_t samples=r32(code,pc); uint8_t ch=r8(code,pc);
        (void)ch;
        if(dst+samples>out.size()) throw std::runtime_error("AudOp::SILENCE_RUN: out of bounds");
        memset(out.data()+dst, 0, samples);
        break;
    }
    case AudOp::LOOP_REF: {
        // [src:8][dst:8][len:4][times:2]
        // src=先頭周期の位置 (RAW済み), dst=2周期目以降の開始, times=コピー回数
        uint64_t src   = r64(code, pc);
        uint64_t dst   = r64(code, pc);
        uint32_t len   = r32(code, pc);
        uint16_t times = r16(code, pc);
        for(uint16_t t = 0; t < times; ++t) {
            uint64_t wpos = dst + (uint64_t)t * len;
            if(wpos + len > out.size()) break;
            memcpy(out.data() + wpos, out.data() + src, len);
        }
        break;
    }
    case AudOp::PHRASE_SAMPLE_REF: {
        uint32_t bid=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        const auto& s2=ctx.blobs.at(bid).data;
        if(len>s2.size()||dst+len>out.size()) throw std::runtime_error("AudOp::PHRASE_SAMPLE_REF: out of bounds");
        memcpy(out.data()+dst, s2.data(), len);
        break;
    }
    case AudOp::CHANNEL_DERIVE: {
        uint8_t src_ch=r8(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc); uint8_t op2=r8(code,pc);
        (void)src_ch;
        if(dst+len>out.size()) throw std::runtime_error("AudOp::CHANNEL_DERIVE: out of bounds");
        if(op2==1) for(uint32_t i=0;i<len;++i) out[dst+i]=(uint8_t)(256-out[dst+i]);
        break;
    }
    case AudOp::ENVELOPE_PATCH: {
        uint64_t src=r64(code,pc); uint32_t env_id=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        (void)env_id;
        if(dst+len>out.size()) throw std::runtime_error("AudOp::ENVELOPE_PATCH: out of bounds");
        if(src+len<=out.size()) memmove(out.data()+dst, out.data()+src, len);
        break;
    }
    default:
        throw std::runtime_error("Unknown AudOp: " + std::to_string(op));
    }
}

// --- アセット系 (0x60〜) ---
// pool から取得し、なければ blobs から取得するヘルパー
static const std::vector<uint8_t>& pool_or_blobs(
    const AssetPool& pool, const std::vector<Blob>& blobs, uint32_t id)
{
    if (id < pool.assets.size()) return pool.assets[id];
    if (id < blobs.size()) return blobs[id].data;
    throw std::runtime_error("pool_or_blobs: id out of range");
}

static void handle_asset(uint8_t op, const std::vector<uint8_t>& code,
                          size_t& pc, std::vector<uint8_t>& out,
                          GenreVmContext& ctx)
{
    switch (op) {
    case AssetOp::ASSET_BASE_REF: {
        // [asset_id:4][dst:8][len:4] — blobs[id] を dst へコピー
        uint32_t id  = r32(code, pc);
        uint64_t dst = r64(code, pc);
        uint32_t len = r32(code, pc);
        const auto& asset = ctx.blobs.at(id).data;
        if (len > asset.size() || dst + len > out.size())
            throw std::runtime_error("AssetOp::ASSET_BASE_REF: out of bounds");
        memcpy(out.data() + dst, asset.data(), len);
        break;
    }
    case AssetOp::VARIANT_DELTA: {
        // [base_id:4][dst:8][delta_bid:4]
        // base_id: 元アセットの参照 (メタ情報・バージョン管理用)
        // delta_bid: 実際に出力するデータ (現アセットそのもの)
        // → delta_bid の内容をそのまま dst に展開
        uint32_t base_id   = r32(code, pc);
        uint64_t dst       = r64(code, pc);
        uint32_t delta_bid = r32(code, pc);
        (void)base_id; // バージョン追跡に使用、VM では参照のみ
        const auto& delta = ctx.blobs.at(delta_bid).data;
        if (dst + delta.size() > out.size())
            throw std::runtime_error("AssetOp::VARIANT_DELTA: out of bounds");
        memcpy(out.data() + dst, delta.data(), delta.size());
        break;
    }
    case AssetOp::RESOLUTION_DERIVE: {
        // [base_id:4][dst:8][scale_num:1][scale_den:1]
        // base_id アセットを scale_num/scale_den 倍にリサイズして dst に書く
        uint32_t base_id   = r32(code, pc);
        uint64_t dst       = r64(code, pc);
        uint8_t  scale_num = r8(code, pc);
        uint8_t  scale_den = r8(code, pc);
        if (scale_den == 0) throw std::runtime_error("RESOLUTION_DERIVE: scale_den=0");
        const auto& src = pool_or_blobs(ctx.pool, ctx.blobs, base_id);
        size_t new_size = src.size() * scale_num / scale_den;
        if (dst + new_size > out.size())
            out.resize(dst + new_size, 0);
        // 近傍補間 (1Dバイト列)
        for (size_t i = 0; i < new_size; ++i) {
            size_t si = i * scale_den / scale_num;
            if (si >= src.size()) si = src.size() - 1;
            out[dst + i] = src[si];
        }
        break;
    }
    case AssetOp::ATLAS_REGION_REF: {
        // [atlas_id:4][x:2][y:2][w:2][h:2][dst:8]
        // アトラステクスチャの矩形領域 (x,y,w,h) を dst に w*h バイトとして書き込む
        uint32_t atlas_id = r32(code, pc);
        uint16_t ax = r16(code, pc), ay = r16(code, pc);
        uint16_t aw = r16(code, pc), ah = r16(code, pc);
        uint64_t dst      = r64(code, pc);
        const auto& atlas = pool_or_blobs(ctx.pool, ctx.blobs, atlas_id);
        // アトラスは線形1D配列 (アトラス幅は meta.extra[0..1] から取得、なければ 256)
        uint16_t atlas_w = ctx.meta.extra.size() >= 2
            ? (uint16_t)(ctx.meta.extra[0] | (ctx.meta.extra[1] << 8))
            : 256;
        size_t copy_len = (size_t)aw * ah;
        if (dst + copy_len > out.size()) out.resize(dst + copy_len, 0);
        for (uint16_t row = 0; row < ah; ++row) {
            size_t src_off = ((size_t)(ay + row) * atlas_w + ax);
            size_t dst_off = dst + (size_t)row * aw;
            for (uint16_t col = 0; col < aw; ++col) {
                size_t so = src_off + col;
                out[dst_off + col] = so < atlas.size() ? atlas[so] : 0;
            }
        }
        break;
    }
    case AssetOp::VERSION_CHAIN: {
        // [prev_id:4][dst:8][delta:4]  前バージョンアセット + XOR差分 → 新バージョン
        uint32_t prev_id = r32(code, pc);
        uint64_t dst     = r64(code, pc);
        uint32_t dlen    = r32(code, pc);
        if (pc + dlen > code.size())
            throw std::runtime_error("AssetOp::VERSION_CHAIN: delta out of bounds");
        const auto& prev = pool_or_blobs(ctx.pool, ctx.blobs, prev_id);
        size_t copy_sz = std::min((size_t)dlen, prev.size());
        if (dst + dlen > out.size()) out.resize(dst + dlen, 0);
        // 前バージョンをコピーしてから delta を XOR 適用
        for (uint32_t i = 0; i < dlen; ++i) {
            uint8_t base = i < prev.size() ? prev[i] : 0;
            out[dst + i] = base ^ code[pc + i];
        }
        pc += dlen;
        break;
    }
    case AssetOp::SHARED_TEXTURE: {
        // [tex_id:4][dst:8][len:4]  共有テクスチャプールから len バイトを dst にコピー
        uint32_t tex_id = r32(code, pc);
        uint64_t dst    = r64(code, pc);
        uint32_t len    = r32(code, pc);
        const auto& tex = pool_or_blobs(ctx.pool, ctx.blobs, tex_id);
        if (dst + len > out.size()) out.resize(dst + len, 0);
        size_t copy_sz = std::min((size_t)len, tex.size());
        std::copy(tex.begin(), tex.begin() + copy_sz, out.data() + dst);
        // 残りはゼロ埋め (コピー済みなら不要)
        if (copy_sz < len)
            std::fill(out.data()+dst+copy_sz, out.data()+dst+len, 0);
        break;
    }
    default:
        throw std::runtime_error("Unknown AssetOp: " + std::to_string(op));
    }
}

// --- データベース系 (0x70〜) ---
static void handle_db(uint8_t op, const std::vector<uint8_t>& code,
                       size_t& pc, std::vector<uint8_t>& out,
                       GenreVmContext& ctx)
{
    switch (op) {
    case DbOp::COLUMN_DICT: {
        uint16_t col_id=r16(code,pc); uint32_t dict_id=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        (void)col_id;
        const auto& d=ctx.blobs.at(dict_id).data;
        if(len>d.size()||dst+len>out.size()) throw std::runtime_error("DbOp::COLUMN_DICT: out of bounds");
        memcpy(out.data()+dst, d.data(), len);
        break;
    }
    case DbOp::SPARSE_MAP: {
        uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc); uint8_t nv=r8(code,pc);
        if(dst+len>out.size()) throw std::runtime_error("DbOp::SPARSE_MAP: out of bounds");
        memset(out.data()+dst, nv, len);
        break;
    }
    case DbOp::ROW_DELTA: {
        uint64_t prev=r64(code,pc); uint64_t dst=r64(code,pc); uint32_t delta_id=r32(code,pc);
        (void)prev;
        const auto& d=ctx.blobs.at(delta_id).data;
        if(dst+d.size()>out.size()) throw std::runtime_error("DbOp::ROW_DELTA: out of bounds");
        memcpy(out.data()+dst, d.data(), d.size());
        break;
    }
    case DbOp::PERIODIC_SERIES: {
        uint64_t dst=r64(code,pc); uint32_t period=r32(code,pc); uint32_t count=r32(code,pc);
        if(dst+(uint64_t)period*count>out.size()) throw std::runtime_error("DbOp::PERIODIC_SERIES: out of bounds");
        for(uint32_t i=1;i<count;++i) memcpy(out.data()+dst+i*period, out.data()+dst, period);
        break;
    }
    case DbOp::SCHEMA_REF: {
        r16(code,pc); r64(code,pc); r32(code,pc);
        break;
    }
    case DbOp::BLOCK_RANGE_REPEAT: {
        // [src:8][dst:8][row_len:4][count:4]
        // src=先頭行位置(RAW済み), dst=2行目以降の開始, count=コピー行数
        uint64_t src=r64(code,pc); uint64_t dst2=r64(code,pc); uint32_t row_len=r32(code,pc); uint32_t count=r32(code,pc);
        for(uint32_t i=0;i<count;++i){
            uint64_t wpos=dst2+(uint64_t)i*row_len;
            if(wpos+row_len>out.size()) break;
            memcpy(out.data()+wpos, out.data()+src, row_len);
        }
        break;
    }
    default:
        throw std::runtime_error("Unknown DbOp: " + std::to_string(op));
    }
}

// --- 混合系 (0x80〜) ---
static void handle_mix(uint8_t op, const std::vector<uint8_t>& code,
                        size_t& pc, std::vector<uint8_t>& out,
                        GenreVmContext& ctx)
{
    switch (op) {
    case MixOp::MANIFEST_REF: {
        uint32_t bid=r32(code,pc); uint64_t dst=r64(code,pc);
        const auto& m=ctx.blobs.at(bid).data;
        if(dst+m.size()>out.size()) throw std::runtime_error("MixOp::MANIFEST_REF: out of bounds");
        memcpy(out.data()+dst, m.data(), m.size());
        break;
    }
    case MixOp::CROSS_FILE_COPY: {
        uint32_t fid=r32(code,pc); uint64_t src=r64(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        const auto& f=ctx.blobs.at(fid).data;
        if(src+len>f.size()||dst+len>out.size()) throw std::runtime_error("MixOp::CROSS_FILE_COPY: out of bounds");
        memcpy(out.data()+dst, f.data()+src, len);
        break;
    }
    case MixOp::SHARED_BLOB_POOL: {
        uint32_t bid=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t len=r32(code,pc);
        const auto& p=ctx.blobs.at(bid).data;
        if(len>p.size()||dst+len>out.size()) throw std::runtime_error("MixOp::SHARED_BLOB_POOL: out of bounds");
        memcpy(out.data()+dst, p.data(), len);
        break;
    }
    case MixOp::DEPENDENCY_LINK: {
        r32(code,pc); r64(code,pc);
        break;
    }
    case MixOp::SNAPSHOT_DELTA: {
        uint32_t base_id=r32(code,pc); uint64_t dst=r64(code,pc); uint32_t delta_id=r32(code,pc);
        (void)base_id;
        const auto& d=ctx.blobs.at(delta_id).data;
        if(dst+d.size()>out.size()) throw std::runtime_error("MixOp::SNAPSHOT_DELTA: out of bounds");
        memcpy(out.data()+dst, d.data(), d.size());
        break;
    }
    default:
        throw std::runtime_error("Unknown MixOp: " + std::to_string(op));
    }
}

// ============================================================
// ジャンル拡張 VM メインループ
// ============================================================
std::vector<uint8_t> run_genre_vm(
    const std::vector<uint8_t>& code,
    const std::vector<Blob>&    blobs,
    const GenreMetadata&        meta)
{
    GenreVmContext ctx{blobs, meta};
    std::vector<uint8_t> out;
    size_t pc = 0;

    while (pc < code.size()) {
        uint8_t op = r8(code, pc);

        // ---- Base DSL (Level 2 互換) ----
        switch (op) {
        case BaseOp::OUTPUT: {
            out.assign(r64(code, pc), 0);
            continue;
        }
        case BaseOp::RAW: {
            uint32_t id  = r32(code, pc);
            uint64_t sof = r64(code, pc);
            uint64_t dst = r64(code, pc);
            uint32_t len = r32(code, pc);
            const auto& b = blobs.at(id).data;
            if (sof + len > b.size() || dst + len > out.size())
                throw std::runtime_error("RAW: out of bounds");
            memcpy(out.data() + dst, b.data() + sof, len);
            continue;
        }
        case BaseOp::COPY: {
            uint64_t s = r64(code, pc);
            uint64_t d = r64(code, pc);
            uint32_t l = r32(code, pc);
            if (s + l > out.size() || d + l > out.size())
                throw std::runtime_error("COPY: out of bounds");
            memmove(out.data() + d, out.data() + s, l);
            continue;
        }
        case BaseOp::FILL: {
            uint64_t dst = r64(code, pc);
            uint32_t len = r32(code, pc);
            uint8_t  val = r8(code,  pc);
            if (dst + len > out.size())
                throw std::runtime_error("FILL: out of bounds");
            memset(out.data() + dst, val, len);
            continue;
        }
        case BaseOp::REF_BLOB: {
            // 別 Blob 全体を dst に展開
            uint32_t id  = r32(code, pc);
            uint64_t dst = r64(code, pc);
            uint32_t len = r32(code, pc);
            const auto& b = blobs.at(id).data;
            if (len > b.size() || dst + len > out.size())
                throw std::runtime_error("REF_BLOB: out of bounds");
            memcpy(out.data() + dst, b.data(), len);
            continue;
        }
        case BaseOp::REF_ASSET: {
            uint32_t id  = r32(code, pc);
            uint64_t dst = r64(code, pc);
            uint32_t len = r32(code, pc);
            const auto& asset = pool_or_blobs(ctx.pool, ctx.blobs, id);
            if (len > asset.size() || dst + len > out.size())
                throw std::runtime_error("REF_ASSET: out of bounds");
            memcpy(out.data() + dst, asset.data(), len);
            continue;
        }
        case BaseOp::APPLY_DELTA: {
            uint64_t src       = r64(code, pc);
            uint32_t delta_bid = r32(code, pc);
            uint64_t dst       = r64(code, pc);
            uint32_t len       = r32(code, pc);
            const auto& delta  = blobs.at(delta_bid).data;
            if (src + len > out.size() || dst + len > out.size() || len > delta.size())
                throw std::runtime_error("APPLY_DELTA: out of bounds");
            for (uint32_t i = 0; i < len; ++i)
                out[dst + i] = out[src + i] ^ delta[i];
            continue;
        }
        case BaseOp::CHECKPOINT:
            r32(code, pc); // label (ログ用、処理は無視)
            continue;
        case BaseOp::GENRE_BEGIN:
            ctx.current_genre = static_cast<Genre>(r8(code, pc));
            continue;
        case BaseOp::GENRE_END:
            ctx.current_genre = Genre::UNKNOWN;
            continue;
        case BaseOp::INLINE_DATA: {
            // [dst:8][len:4][data:len]
            uint64_t dst = r64(code, pc);
            uint32_t len = r32(code, pc);
            if (pc + len > code.size())
                throw std::runtime_error("INLINE_DATA: out of code bounds");
            if (dst + len > out.size()) out.resize(dst + len, 0);
            std::copy(code.data()+pc, code.data()+pc+len, out.data()+dst);
            pc += len;
            continue;  // ← break → continue に修正
        }
        case BaseOp::END:
            return out;
        default:
            break; // ジャンル命令へ
        }

        // ---- ジャンル命令ディスパッチ ----
        if      (op >= 0x20 && op <= 0x2F) handle_doc  (op,code,pc,out,ctx);
        else if (op >= 0x30 && op <= 0x3F) handle_img  (op,code,pc,out,ctx);
        else if (op >= 0x40 && op <= 0x4F) handle_vid  (op,code,pc,out,ctx);
        else if (op >= 0x50 && op <= 0x5F) handle_aud  (op,code,pc,out,ctx);
        else if (op >= 0x60 && op <= 0x6F) handle_asset(op,code,pc,out,ctx);
        else if (op >= 0x70 && op <= 0x7F) handle_db   (op,code,pc,out,ctx);
        else if (op >= 0x80 && op <= 0x8F) handle_mix  (op,code,pc,out,ctx);
        else
            throw std::runtime_error("genre_vm: unknown opcode 0x"
                + std::to_string(op));
    }
    return out;
}

#endif // GENRE_DSL_VM_CPP_INCLUDED
