// frame_index.cpp — CPB Level 1 フレームインデックス実装
#include "frame_index.hpp"
#include "cpb_helpers.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>

// ============================================================
// Adler-32 (高速ハッシュ)
// ============================================================
static uint32_t adler32(const uint8_t* data, size_t len)
{
    uint32_t a=1, b=0;
    for(size_t i=0;i<len;++i){
        a=(a+data[i])%65521;
        b=(b+a)%65521;
    }
    return (b<<16)|a;
}

// ============================================================
// シリアライズ ヘルパー
// ============================================================


// ============================================================
// シリアライズ / デシリアライズ
// ============================================================
// フォーマット:
//   magic[4] = "FIDX"
//   version(4) frame_count(4) total_bytes(8) fps(8=double)
//   === FrameDirectory ===
//   for each frame:
//     frame_id(4) data_offset(8) data_size(4) hash32(4) label[32]
//   === TrigramIndex ===
//   trigram_count(4)
//   for each entry:
//     key(4) posting_count(4)
//     for each posting: frame_id(4) offset(4)
//   === LabelIndex ===
//   label_count(4)
//   for each entry:
//     label_string(str) frame_count(4) frame_ids(4*n)

std::vector<uint8_t> serialize_index(const FrameIndex& idx)
{
    std::vector<uint8_t> out;
    out.reserve(1024 + idx.directory.size()*52 + idx.trigram.size()*12);

    // magic + header
    out.insert(out.end(), {'F','I','D','X'});
    w32(out, idx.version);
    w32(out, idx.frame_count);
    w64(out, idx.total_bytes);
    uint64_t fps_bits; memcpy(&fps_bits, &idx.fps, 8);
    w64(out, fps_bits);

    // FrameDirectory
    for(auto& e : idx.directory){
        w32(out, e.frame_id);
        w64(out, e.data_offset);
        w32(out, e.data_size);
        w32(out, e.hash32);
        out.insert(out.end(), e.label, e.label+32);
    }

    // TrigramIndex
    w32(out, (uint32_t)idx.trigram.size());
    for(auto& [key, posts] : idx.trigram){
        w32(out, key);
        w32(out, (uint32_t)posts.size());
        for(auto& [fid, off] : posts){
            w32(out, fid);
            w32(out, off);
        }
    }

    // LabelIndex
    w32(out, (uint32_t)idx.labels.size());
    for(auto& [lbl, fids] : idx.labels){
        wstr(out, lbl);
        w32(out, (uint32_t)fids.size());
        for(auto fid : fids) w32(out, fid);
    }

    return out;
}

FrameIndex deserialize_index(const std::vector<uint8_t>& buf)
{
    if(buf.size() < 8) throw std::runtime_error("FrameIndex: buffer too short");
    const uint8_t* p = buf.data();

    if(memcmp(p,"FIDX",4)!=0) throw std::runtime_error("FrameIndex: bad magic");
    p += 4;

    FrameIndex idx;
    idx.version     = r32(p);
    idx.frame_count = r32(p);
    idx.total_bytes = r64(p);
    uint64_t fps_bits = r64(p);
    memcpy(&idx.fps, &fps_bits, 8);

    // FrameDirectory
    idx.directory.resize(idx.frame_count);
    for(auto& e : idx.directory){
        e.frame_id    = r32(p);
        e.data_offset = r64(p);
        e.data_size   = r32(p);
        e.hash32      = r32(p);
        memcpy(e.label, p, 32); p+=32;
    }

    // TrigramIndex
    uint32_t tcount = r32(p);
    for(uint32_t i=0;i<tcount;++i){
        uint32_t key   = r32(p);
        uint32_t pcount= r32(p);
        auto& posts = idx.trigram[key];
        posts.reserve(pcount);
        for(uint32_t j=0;j<pcount;++j){
            uint32_t fid = r32(p);
            uint32_t off = r32(p);
            posts.push_back({fid, off});
        }
    }

    // LabelIndex
    uint32_t lcount = r32(p);
    for(uint32_t i=0;i<lcount;++i){
        std::string lbl = rstr(p);
        uint32_t fcount = r32(p);
        auto& fids = idx.labels[lbl];
        fids.reserve(fcount);
        for(uint32_t j=0;j<fcount;++j)
            fids.push_back(r32(p));
    }

    return idx;
}

// ============================================================
// インデックス構築
// ============================================================
// Trigram の最大エントリ数 (巨大データでインデックスが肥大化しないように)
static constexpr size_t MAX_TRIGRAM_PER_FRAME = 1024;

FrameIndex build_index(
    const std::vector<std::vector<uint8_t>>& blobs,
    double fps,
    const std::vector<std::string>& labels)
{
    FrameIndex idx;
    idx.fps         = fps;
    idx.frame_count = (uint32_t)blobs.size();

    uint64_t offset = 0;
    for(uint32_t fid = 0; fid < (uint32_t)blobs.size(); ++fid){
        const auto& blob = blobs[fid];

        // FrameEntry
        FrameEntry e;
        e.frame_id    = fid;
        e.data_offset = offset;
        e.data_size   = (uint32_t)blob.size();
        e.hash32      = adler32(blob.data(), blob.size());
        if(fid < labels.size()){
            size_t llen = std::min(labels[fid].size(), size_t(31));
            memcpy(e.label, labels[fid].data(), llen);
        }
        idx.directory.push_back(e);

        // TrigramIndex: 3バイトNgramをポスティングリストに追加
        size_t n_trigrams = 0;
        if(blob.size() >= 3){
            for(size_t i=0; i+3<=blob.size() && n_trigrams<MAX_TRIGRAM_PER_FRAME; ++i){
                uint32_t key = ((uint32_t)blob[i]<<16)
                             | ((uint32_t)blob[i+1]<<8)
                             | (uint32_t)blob[i+2];
                auto& posts = idx.trigram[key];
                // 同じフレームで重複登録しない (先頭オフセットのみ)
                if(posts.empty() || posts.back().first != fid){
                    posts.push_back({fid, (uint32_t)i});
                    ++n_trigrams;
                }
            }
        }

        // LabelIndex
        if(fid < labels.size() && !labels[fid].empty())
            idx.labels[labels[fid]].push_back(fid);

        offset += blob.size();
    }

    idx.total_bytes = offset;
    return idx;
}

// ============================================================
// Boyer-Moore-Horspool パターン検索
// ============================================================
static std::vector<size_t> bmh_search(
    const uint8_t* text, size_t tlen,
    const uint8_t* pat,  size_t plen)
{
    std::vector<size_t> hits;
    if(plen == 0 || plen > tlen) return hits;

    // Bad-character table
    size_t skip[256];
    std::fill(skip, skip+256, plen);
    for(size_t i=0;i<plen-1;++i)
        skip[pat[i]] = plen-1-i;

    size_t i = plen-1;
    while(i < tlen){
        size_t j=plen-1, k=i;
        while(j < plen && text[k]==pat[j]){ --j; --k; }
        if(j==size_t(-1)) hits.push_back(k+1);
        i += skip[text[i]];
    }
    return hits;
}

// ============================================================
// 検索API
// ============================================================
std::vector<SearchHit> search_pattern(
    const FrameIndex& idx,
    const std::vector<std::vector<uint8_t>>& blobs,
    const std::vector<uint8_t>& pattern)
{
    std::vector<SearchHit> results;
    if(pattern.empty()) return results;

    // Trigram フィルタ: パターン先頭3バイトでフレーム候補を絞る
    std::vector<uint32_t> candidates;
    if(pattern.size() >= 3){
        uint32_t key = ((uint32_t)pattern[0]<<16)
                     | ((uint32_t)pattern[1]<<8)
                     | (uint32_t)pattern[2];
        auto it = idx.trigram.find(key);
        if(it == idx.trigram.end()) return results; // 候補ゼロ
        for(auto& [fid, off] : it->second)
            candidates.push_back(fid);
    } else {
        // パターンが短い場合は全フレームをスキャン
        for(uint32_t i=0;i<idx.frame_count;++i) candidates.push_back(i);
    }

    // 候補フレームを実際にスキャン
    for(uint32_t fid : candidates){
        if(fid >= blobs.size()) continue;
        const auto& blob = blobs[fid];
        auto hits = bmh_search(blob.data(), blob.size(),
                               pattern.data(), pattern.size());
        for(auto off : hits){
            SearchHit h;
            h.frame_id    = fid;
            h.byte_offset = idx.directory[fid].data_offset + off;
            h.match_len   = (uint32_t)pattern.size();
            results.push_back(h);
        }
    }
    return results;
}

std::vector<SearchHit> search_text(
    const FrameIndex& idx,
    const std::vector<std::vector<uint8_t>>& blobs,
    const std::string& query)
{
    std::vector<uint8_t> pat(query.begin(), query.end());
    return search_pattern(idx, blobs, pat);
}

std::vector<uint32_t> search_label(
    const FrameIndex& idx,
    const std::string& label)
{
    auto it = idx.labels.find(label);
    if(it == idx.labels.end()) return {};
    return it->second;
}

// ============================================================
// 時間軸アクセス
// ============================================================
uint32_t time_to_frame(const FrameIndex& idx, double sec)
{
    uint32_t fid = (uint32_t)(sec * idx.fps);
    return std::min(fid, idx.frame_count > 0 ? idx.frame_count-1 : 0u);
}

double frame_to_time(const FrameIndex& idx, uint32_t frame_id)
{
    return frame_id / idx.fps;
}

std::vector<uint32_t> range_query(
    const FrameIndex& idx,
    double time_start_sec,
    double time_end_sec)
{
    uint32_t start = time_to_frame(idx, time_start_sec);
    uint32_t end   = std::min(time_to_frame(idx, time_end_sec),
                              idx.frame_count > 0 ? idx.frame_count-1 : 0u);
    std::vector<uint32_t> result;
    for(uint32_t i=start; i<=end; ++i) result.push_back(i);
    return result;
}
