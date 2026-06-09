#include "container.hpp"
#include "cpb_helpers.hpp"
#ifndef NO_FRAME_IO
#include "frame_io.hpp"
#endif

#include <fstream>
#include <stdexcept>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>

// ============================================================
// ファイル I/O
// ============================================================
std::vector<uint8_t> load_bytes(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("Failed to open input file: " + path);
    ifs.seekg(0, std::ios::end);
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !ifs.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("Failed to read input file: " + path);
    return data;
}

void save_bytes(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) throw std::runtime_error("Failed to open output file: " + path);
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs) throw std::runtime_error("Failed to write output file: " + path);
    }
}

// ============================================================
// Blob 分割
// RS(255,223) 後: 8 + ceil(CHUNK/223)*255 ≈ 1.14MB < フレーム容量 1.98MB
// ============================================================
std::vector<Blob> split_into_blobs(const std::vector<uint8_t>& data)
{
    constexpr size_t CHUNK = 1024 * 1024; // 1MB
    std::vector<Blob> blobs;
    for (size_t i = 0; i < data.size(); i += CHUNK) {
        Blob b;
        b.data.assign(data.begin() + i,
                      data.begin() + std::min(i + CHUNK, data.size()));
        blobs.push_back(std::move(b));
    }
    return blobs;
}

// ============================================================
// DSL バイト列書き込みヘルパー
// ============================================================

// ============================================================
// DSL 命令サイズ定数
//   RAW  = 1(op) + 4(blob_id) + 8(src_offset) + 8(dst) + 4(len) = 25 bytes
//   COPY = 1(op) + 8(src) + 8(dst) + 4(len)             = 21 bytes
// COPY が RAW より得するのは: match_len > 25 bytes
// (25バイト以上一致すれば RAW×2 より COPY のほうが小さい)
// ============================================================
static constexpr size_t RAW_INSTR_SIZE  = 25;
static constexpr size_t COPY_INSTR_SIZE = 21;
static constexpr size_t FILL_INSTR_SIZE = 14; // 1(op)+8(dst)+4(len)+1(byte)
static constexpr size_t MIN_MATCH       = RAW_INSTR_SIZE + 1; // 26 bytes
static constexpr size_t MIN_FILL        = FILL_INSTR_SIZE + 1; // 15 bytes同一
static constexpr size_t MAX_MATCH       = 0xFFFF'FFFF;         // uint32 max

// ============================================================
// ハッシュチェーン LZ77
// ============================================================
// 4バイトハッシュ
static inline uint32_t hash4(const uint8_t* p)
{
    uint32_t v; std::memcpy(&v, p, 4);
    return (v * 2654435761u) >> 16; // Knuth multiplicative hash
}

static constexpr size_t HASH_SIZE = 1 << 16; // 65536 バケット
static constexpr size_t CHAIN_LEN = 256;      // チェーンを最大 256 個たどる

// [src_offset, len] のマッチ結果
struct Match { size_t src; size_t len; };

// flat バッファ上で最長一致を探す
static Match find_best_match(
    const uint8_t* buf, size_t buf_size,
    size_t pos,
    const std::vector<size_t>& head,  // head[hash] = 最新位置
    const std::vector<size_t>& prev)  // prev[pos]  = 同ハッシュの前の位置
{
    Match best{0, 0};
    if (pos + 4 > buf_size) return best;

    uint32_t h = hash4(buf + pos) & (HASH_SIZE - 1);
    size_t   cur = head[h];

    for (int chain = 0; chain < CHAIN_LEN && cur != SIZE_MAX && cur < pos; ++chain) {
        // 一致長を測る
        size_t max_len = std::min(buf_size - pos, pos - cur); // src が dst に追いつかない
        max_len = std::min(max_len, MAX_MATCH);
        size_t len = 0;
        while (len < max_len && buf[cur + len] == buf[pos + len]) ++len;

        if (len >= MIN_MATCH && len > best.len)
            best = {cur, len};

        cur = prev[cur];
    }
    return best;
}

// ============================================================
// build_dsl  — LZ77 スキャン付き
//
// blobs を結合した「仮想フラットバッファ」上でマッチを探す。
// マッチが見つかれば COPY 命令、そうでなければ RAW 命令を発行。
//
// RAW 命令は Blob 境界をまたがないように分割して発行する。
// (VM は blob.data() を直接参照するため blob 内 offset が必要)
// ============================================================
std::vector<uint8_t> build_dsl(const std::vector<Blob>& blobs, size_t total_size)
{
    // ---- 1. フラットバッファを構築 ----
    std::vector<uint8_t> flat;
    flat.reserve(total_size);
    for (auto& b : blobs) flat.insert(flat.end(), b.data.begin(), b.data.end());
    const size_t N = flat.size();

    // ---- 2. Blob オフセット表 ----
    // blob_offset[i] = flat バッファ上での blobs[i] の開始位置
    std::vector<size_t> blob_off(blobs.size() + 1, 0);
    for (size_t i = 0; i < blobs.size(); ++i)
        blob_off[i + 1] = blob_off[i] + blobs[i].data.size();

    // pos → (blob_id, offset_in_blob) の逆引き
    auto to_blob_ref = [&](size_t pos) -> std::pair<uint32_t, size_t> {
        // 二分探索
        size_t lo = 0, hi = blobs.size();
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (blob_off[mid] <= pos) lo = mid; else hi = mid;
        }
        return {static_cast<uint32_t>(lo), pos - blob_off[lo]};
    };

    // ---- 3. ハッシュチェーン初期化 ----
    std::vector<size_t> head(HASH_SIZE, SIZE_MAX);
    std::vector<size_t> prev(N,        SIZE_MAX);

    // ---- 4. DSL バイト列生成 ----
    std::vector<uint8_t> dsl;
    dsl.reserve(N / 8); // 目安

    // OUTPUT 命令
    w8(dsl, 0x01);
    w64(dsl, static_cast<uint64_t>(total_size));

    size_t pos = 0;

    // RAW を発行する小関数 (Blob をまたがない範囲で分割)
    auto emit_raw = [&](size_t flat_pos, size_t len) {
        size_t remaining = len;
        size_t cur       = flat_pos;
        while (remaining > 0) {
            auto [bid, off_in_blob] = to_blob_ref(cur);
            size_t available = blobs[bid].data.size() - off_in_blob;
            size_t emit_len  = std::min(remaining, available);
            // RAW: op(1) + blob_id(4) + src_offset(8) + dst(8) + len(4) = 25 bytes
            w8(dsl, 0x02);
            w32(dsl, bid);
            w64(dsl, static_cast<uint64_t>(off_in_blob));
            w64(dsl, static_cast<uint64_t>(cur));
            w32(dsl, static_cast<uint32_t>(emit_len));
            cur       += emit_len;
            remaining -= emit_len;
        }
    };

    // FILL 候補を検出して発行する小関数
    // flat[pos..] で同一バイトが何バイト続くかを返す
    auto fill_run = [&](size_t p) -> size_t {
        uint8_t v = flat[p];
        size_t  n = 1;
        while (p + n < N && flat[p + n] == v) ++n;
        return n;
    };

    // ハッシュ登録ヘルパー
    auto insert_hash = [&](size_t p) {
        if (p + 4 <= N) {
            uint32_t h = hash4(flat.data() + p) & (HASH_SIZE - 1);
            prev[p] = head[h];
            head[h] = p;
        }
    };

    while (pos < N) {
        // ① FILL 優先: 同一バイトが MIN_FILL 以上続くなら即 FILL
        //    FILL 命令 (14B) < COPY 命令 (21B) なので常に得
        {
            size_t run = fill_run(pos);
            if (run >= MIN_FILL) {
                w8(dsl, 0x04);
                w64(dsl, static_cast<uint64_t>(pos));
                w32(dsl, static_cast<uint32_t>(run));
                w8(dsl, flat[pos]);
                // FILL 区間もハッシュ登録して進む
                for (size_t k = 0; k < run; ++k) insert_hash(pos + k);
                pos += run;
                continue;
            }
        }

        // ② LZ77 COPY 検索
        Match m = (pos + 4 <= N)
                  ? find_best_match(flat.data(), N, pos, head, prev)
                  : Match{0, 0};

        if (m.len >= MIN_MATCH) {
            // COPY 命令
            w8(dsl, 0x03);
            w64(dsl, static_cast<uint64_t>(m.src));
            w64(dsl, static_cast<uint64_t>(pos));
            w32(dsl, static_cast<uint32_t>(m.len));
            for (size_t k = 0; k < m.len; ++k) insert_hash(pos + k);
            pos += m.len;
        } else {
            // ③ RAW: 次の FILL/COPY 候補まで溜めて発行
            size_t raw_start = pos;
            insert_hash(pos);
            ++pos;

            while (pos < N) {
                // 次が FILL 候補なら切る
                if (fill_run(pos) >= MIN_FILL) break;
                // 次が COPY 候補なら切る
                Match m2 = (pos + 4 <= N)
                           ? find_best_match(flat.data(), N, pos, head, prev)
                           : Match{0, 0};
                if (m2.len >= MIN_MATCH) break;
                insert_hash(pos);
                ++pos;
            }
            emit_raw(raw_start, pos - raw_start);
        }
    }

    // END 命令
    w8(dsl, 0xFF);
    return dsl;
}
