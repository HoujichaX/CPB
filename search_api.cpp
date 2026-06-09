// search_api.cpp — CPB Level1 外部検索エンジン実装
#include "search_api.hpp"
#include "frame_index.hpp"
#include <chrono>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <cstring>

using Clock = std::chrono::high_resolution_clock;
static double ms_now(Clock::time_point t0){
    return std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
}

// ============================================================
// SearchEngine
// ============================================================
void SearchEngine::load_index(const FrameIndex& idx){ idx_ = idx; }

void SearchEngine::load_blobs(const std::vector<std::vector<uint8_t>>& blobs){
    blobs_ = blobs; has_blobs_ = true;
}

SearchEngine::IndexInfo SearchEngine::info() const {
    IndexInfo i;
    i.frame_count     = idx_.frame_count;
    i.total_bytes     = idx_.total_bytes;
    i.fps             = idx_.fps;
    i.duration_sec    = idx_.fps > 0 ? idx_.frame_count / idx_.fps : 0.0;
    i.trigram_entries = idx_.trigram.size();
    i.label_count     = idx_.labels.size();
    i.index_size_bytes= serialize_index(idx_).size();
    return i;
}

std::vector<uint8_t> SearchEngine::export_index() const {
    return serialize_index(idx_); // Blobデータは含まない
}

void SearchEngine::import_index(const std::vector<uint8_t>& buf){
    idx_ = deserialize_index(buf);
}

// ── ヒット → SearchResult 変換ヘルパー ──
static SearchResult make_result(
    const SearchHit& h,
    const FrameIndex& idx,
    QueryType qt)
{
    SearchResult r;
    r.frame_id    = h.frame_id;
    r.byte_offset = h.byte_offset;
    r.match_len   = h.match_len;
    r.matched_by  = qt;
    if(h.frame_id < idx.directory.size()){
        const auto& e = idx.directory[h.frame_id];
        r.hash32     = e.hash32;
        r.label      = std::string((const char*)e.label,
                           strnlen((const char*)e.label,32));
        r.timestamp  = frame_to_time(idx, h.frame_id);
    }
    return r;
}

// ── パターン / テキスト検索 ──
SearchResponse SearchEngine::search_pattern_impl(const Query& q) const
{
    SearchResponse resp;
    resp.total_frames = idx_.frame_count;
    if(!has_blobs_){
        resp.query_summary = "Blob未ロード: パターン検索不可 (ラベル/時間/hash検索は可能)";
        return resp;
    }
    auto t0 = Clock::now();

    std::vector<uint8_t> pat = (q.type==QueryType::TEXT)
        ? std::vector<uint8_t>(q.text.begin(), q.text.end())
        : q.pattern;

    auto hits = search_pattern(idx_, blobs_, pat);
    resp.scanned_frames = (uint32_t)(
        pat.size() >= 3 ? [&]{
            uint32_t key=((uint32_t)pat[0]<<16)|((uint32_t)pat[1]<<8)|pat[2];
            auto it=idx_.trigram.find(key);
            return it==idx_.trigram.end() ? 0u
                : (uint32_t)it->second.size();
        }() : idx_.frame_count);

    for(auto& h : hits)
        resp.hits.push_back(make_result(h, idx_, q.type));

    resp.elapsed_ms    = ms_now(t0);
    resp.query_summary = query_to_string(q)
        + " → " + std::to_string(resp.hits.size()) + " hits";
    return resp;
}

// ── ラベル検索 ──
SearchResponse SearchEngine::search_label_impl(const Query& q) const
{
    SearchResponse resp;
    resp.total_frames = idx_.frame_count;
    auto t0 = Clock::now();

    auto fids = search_label(idx_, q.label);
    resp.scanned_frames = 1; // O(1)

    for(auto fid : fids){
        SearchResult r;
        r.frame_id   = fid;
        r.matched_by = QueryType::LABEL;
        r.match_len  = (uint32_t)q.label.size();
        if(fid < idx_.directory.size()){
            const auto& e = idx_.directory[fid];
            r.hash32     = e.hash32;
            r.byte_offset= e.data_offset;
            r.label      = std::string((const char*)e.label,
                               strnlen((const char*)e.label,32));
            r.timestamp  = frame_to_time(idx_, fid);
        }
        resp.hits.push_back(r);
    }
    resp.elapsed_ms    = ms_now(t0);
    resp.query_summary = query_to_string(q)
        + " → " + std::to_string(resp.hits.size()) + " hits";
    return resp;
}

// ── 時間範囲検索 ──
SearchResponse SearchEngine::search_time_impl(const Query& q) const
{
    SearchResponse resp;
    resp.total_frames = idx_.frame_count;
    auto t0 = Clock::now();

    auto fids = range_query(idx_, q.time_start, q.time_end);
    resp.scanned_frames = (uint32_t)fids.size();

    for(auto fid : fids){
        SearchResult r;
        r.frame_id   = fid;
        r.matched_by = QueryType::TIME_RANGE;
        if(fid < idx_.directory.size()){
            const auto& e = idx_.directory[fid];
            r.hash32     = e.hash32;
            r.byte_offset= e.data_offset;
            r.match_len  = e.data_size;
            r.label      = std::string((const char*)e.label,
                               strnlen((const char*)e.label,32));
            r.timestamp  = frame_to_time(idx_, fid);
        }
        resp.hits.push_back(r);
    }
    resp.elapsed_ms    = ms_now(t0);
    resp.query_summary = query_to_string(q)
        + " → " + std::to_string(resp.hits.size()) + " hits";
    return resp;
}

// ── Hash32 直接検索 ──
SearchResponse SearchEngine::search_hash_impl(const Query& q) const
{
    SearchResponse resp;
    resp.total_frames = idx_.frame_count;
    auto t0 = Clock::now();

    for(uint32_t fid=0; fid<idx_.frame_count; ++fid){
        if(fid >= idx_.directory.size()) break;
        if(idx_.directory[fid].hash32 == q.hash32){
            SearchResult r;
            r.frame_id   = fid;
            r.matched_by = QueryType::HASH32;
            r.hash32     = q.hash32;
            const auto& e = idx_.directory[fid];
            r.byte_offset= e.data_offset;
            r.match_len  = e.data_size;
            r.label      = std::string((const char*)e.label,
                               strnlen((const char*)e.label,32));
            r.timestamp  = frame_to_time(idx_, fid);
            resp.hits.push_back(r);
        }
    }
    resp.scanned_frames = idx_.frame_count;
    resp.elapsed_ms    = ms_now(t0);
    resp.query_summary = query_to_string(q)
        + " → " + std::to_string(resp.hits.size()) + " hits";
    return resp;
}

// ── AND合成: 全子クエリにヒットしたframe_idのみ残す ──
SearchResponse SearchEngine::merge_and(
    const std::vector<SearchResponse>& rs) const
{
    if(rs.empty()) return {};
    SearchResponse result = rs[0];

    for(size_t i=1; i<rs.size(); ++i){
        std::unordered_set<uint32_t> keep;
        for(auto& h : rs[i].hits) keep.insert(h.frame_id);

        std::vector<SearchResult> filtered;
        for(auto& h : result.hits)
            if(keep.count(h.frame_id)) filtered.push_back(h);
        result.hits = std::move(filtered);
        result.elapsed_ms += rs[i].elapsed_ms;
    }
    return result;
}

// ── OR合成: 重複frame_idを除去して統合 ──
SearchResponse SearchEngine::merge_or(
    const std::vector<SearchResponse>& rs) const
{
    if(rs.empty()) return {};
    SearchResponse result;
    std::unordered_set<uint64_t> seen; // frame_id + byte_offset

    for(auto& r : rs){
        result.elapsed_ms += r.elapsed_ms;
        for(auto& h : r.hits){
            uint64_t key = ((uint64_t)h.frame_id << 32) | h.byte_offset;
            if(seen.insert(key).second)
                result.hits.push_back(h);
        }
    }
    result.total_frames   = rs[0].total_frames;
    result.scanned_frames = rs[0].scanned_frames;
    return result;
}

// ── メインディスパッチ ──
SearchResponse SearchEngine::search(const Query& q) const
{
    switch(q.type){
    case QueryType::PATTERN:    return search_pattern_impl(q);
    case QueryType::TEXT:       return search_pattern_impl(q);
    case QueryType::LABEL:      return search_label_impl(q);
    case QueryType::TIME_RANGE: return search_time_impl(q);
    case QueryType::HASH32:     return search_hash_impl(q);
    case QueryType::AND: {
        std::vector<SearchResponse> rs;
        for(auto& c : q.children) rs.push_back(search(c));
        auto r = merge_and(rs);
        r.query_summary = query_to_string(q)
            + " → " + std::to_string(r.hits.size()) + " hits";
        return r;
    }
    case QueryType::OR: {
        std::vector<SearchResponse> rs;
        for(auto& c : q.children) rs.push_back(search(c));
        auto r = merge_or(rs);
        r.query_summary = query_to_string(q)
            + " → " + std::to_string(r.hits.size()) + " hits";
        return r;
    }
    }
    return {};
}

// ── クエリ → 文字列 ──
std::string query_to_string(const Query& q)
{
    std::ostringstream oss;
    switch(q.type){
    case QueryType::PATTERN:
        oss << "PATTERN[";
        for(auto b:q.pattern) oss<<std::hex<<(int)b<<" ";
        oss<<"]"; break;
    case QueryType::TEXT:
        oss << "TEXT[\"" << q.text << "\"]"; break;
    case QueryType::LABEL:
        oss << "LABEL[\"" << q.label << "\"]"; break;
    case QueryType::TIME_RANGE:
        oss << "TIME[" << q.time_start << "s-" << q.time_end << "s]"; break;
    case QueryType::HASH32:
        oss << "HASH32[0x" << std::hex << q.hash32 << "]"; break;
    case QueryType::AND:
        oss << "AND(";
        for(size_t i=0;i<q.children.size();++i){
            if(i) oss<<", ";
            oss<<query_to_string(q.children[i]);
        }
        oss<<")"; break;
    case QueryType::OR:
        oss << "OR(";
        for(size_t i=0;i<q.children.size();++i){
            if(i) oss<<", ";
            oss<<query_to_string(q.children[i]);
        }
        oss<<")"; break;
    }
    return oss.str();
}
