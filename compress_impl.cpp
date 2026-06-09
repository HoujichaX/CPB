// compress_impl.cpp — CPB Level 2 プラガブル圧縮実装
// 外部依存: Zstd のみ (他は純C++実装)
#include "compress_iface.hpp"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <unordered_map>

// ============================================================
// Zstd (既存ラッパーを流用 / テスト時はスタブ)
// ============================================================
#ifndef CPB_NO_ZSTD
#  include <zstd.h>
static std::vector<uint8_t> zstd_compress_impl(
    const std::vector<uint8_t>& in, int level)
{
    size_t bound = ZSTD_compressBound(in.size());
    std::vector<uint8_t> out(bound);
    size_t r = ZSTD_compress(out.data(), bound,
                             in.data(), in.size(), level);
    if (ZSTD_isError(r))
        throw std::runtime_error("ZSTD compress failed");
    out.resize(r);
    return out;
}
static std::vector<uint8_t> zstd_decompress_impl(
    const std::vector<uint8_t>& in, size_t hint)
{
    // hint が 0 ならフレームから推測
    uint64_t orig = hint ? hint
        : ZSTD_getFrameContentSize(in.data(), in.size());
    if (orig == ZSTD_CONTENTSIZE_UNKNOWN ||
        orig == ZSTD_CONTENTSIZE_ERROR)
        orig = in.size() * 4; // フォールバック
    std::vector<uint8_t> out(orig);
    size_t r = ZSTD_decompress(out.data(), orig,
                               in.data(), in.size());
    if (ZSTD_isError(r))
        throw std::runtime_error("ZSTD decompress failed");
    out.resize(r);
    return out;
}
#else
// スタブ: プレースホルダー (下部で実装)
static std::vector<uint8_t> zstd_compress_impl(const std::vector<uint8_t>&, int);
static std::vector<uint8_t> zstd_decompress_impl(const std::vector<uint8_t>&, size_t);
#endif

// ============================================================
// LZ4 — 純C++軽量実装 (LZ4ブロック形式互換)
// ============================================================
// LZ4ブロックフォーマット:
//   token(1B): [literal_len:4][match_len:4]
//   literal bytes
//   offset(2B LE) : 一致位置のバックオフセット
//   extra match len bytes (if match_len==15)
namespace lz4 {

static constexpr int  HASH_BITS  = 16;
static constexpr int  HASH_SIZE  = 1 << HASH_BITS;
static constexpr int  MIN_MATCH  = 4;
static constexpr int  LAST_BYTES = 5; // 末尾5バイトはコピーのみ

static inline uint32_t hash4(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4);
    return (v * 0x9E3779B1u) >> (32 - HASH_BITS);
}

static std::vector<uint8_t> compress(const std::vector<uint8_t>& src)
{
    const int n = (int)src.size();
    std::vector<uint8_t> dst;
    dst.reserve(n + n/255 + 16);

    std::array<int, HASH_SIZE> ht;
    ht.fill(-1);

    int anchor = 0, pos = 0;

    auto emit_literals = [&](int lit_start, int lit_len,
                              int match_off, int match_len) {
        // token
        int ll = std::min(lit_len, 15);
        int ml = (match_len > 0) ? std::min(match_len - MIN_MATCH, 15) : 0;
        dst.push_back((uint8_t)((ll << 4) | ml));
        // extra literal length
        if (lit_len >= 15) {
            int extra = lit_len - 15;
            while (extra >= 255) { dst.push_back(255); extra -= 255; }
            dst.push_back((uint8_t)extra);
        }
        // literals
        for (int i = 0; i < lit_len; ++i)
            dst.push_back(src[lit_start + i]);
        if (match_len > 0) {
            // offset LE
            dst.push_back((uint8_t)(match_off & 0xFF));
            dst.push_back((uint8_t)(match_off >> 8));
            // extra match length
            if (match_len - MIN_MATCH >= 15) {
                int extra = match_len - MIN_MATCH - 15;
                while (extra >= 255) { dst.push_back(255); extra -= 255; }
                dst.push_back((uint8_t)extra);
            }
        }
    };

    while (pos < n - LAST_BYTES) {
        uint32_t h = hash4(src.data() + pos);
        int ref = ht[h];
        ht[h] = pos;

        bool matched = false;
        if (ref >= 0 && pos - ref < 65535 &&
            std::memcmp(src.data() + ref, src.data() + pos, MIN_MATCH) == 0)
        {
            // 一致長を延ばす
            int mlen = MIN_MATCH;
            int max_mlen = std::min(n - LAST_BYTES - pos, 65535 - MIN_MATCH);
            while (mlen < max_mlen &&
                   src[ref + mlen] == src[pos + mlen]) ++mlen;

            emit_literals(anchor, pos - anchor, pos - ref, mlen);
            pos   += mlen;
            anchor = pos;
            matched = true;
        }
        if (!matched) ++pos;
    }

    // 残り literal
    emit_literals(anchor, n - anchor, 0, 0);
    return dst;
}

static std::vector<uint8_t> decompress(
    const std::vector<uint8_t>& src, size_t hint)
{
    std::vector<uint8_t> dst;
    dst.reserve(hint ? hint : src.size() * 3);

    size_t ip = 0;
    while (ip < src.size()) {
        uint8_t token = src[ip++];
        int ll = token >> 4;
        int ml = token & 0x0F;

        // literal length
        if (ll == 15) {
            uint8_t extra;
            do { extra = src[ip++]; ll += extra; } while (extra == 255);
        }
        // literals
        for (int i = 0; i < ll; ++i) dst.push_back(src[ip++]);
        if (ip >= src.size()) break; // last sequence has no match

        // offset
        uint16_t off = (uint16_t)(src[ip] | (src[ip+1] << 8)); ip += 2;
        // match length
        if (ml == 15) {
            uint8_t extra;
            do { extra = src[ip++]; ml += extra; } while (extra == 255);
        }
        ml += MIN_MATCH;
        size_t match_pos = dst.size() - off;
        for (int i = 0; i < ml; ++i)
            dst.push_back(dst[match_pos + i]);
    }
    return dst;
}

} // namespace lz4

// ============================================================
// Delta encoding — 前バイトとの差分
// ============================================================
// フォーマット: [orig_size:8LE] [delta bytes...]
// 8バイト境界ごとに独立した基底値を保持 (stride=1)
namespace delta {

static std::vector<uint8_t> encode(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    out.reserve(8 + in.size());
    uint64_t sz = in.size();
    for(int i=0;i<8;++i) out.push_back((uint8_t)((sz>>(i*8))&0xFF));
    uint8_t prev = 0;
    for(auto b : in){ out.push_back((uint8_t)(b - prev)); prev = b; }
    return out;
}

static std::vector<uint8_t> decode(const std::vector<uint8_t>& in)
{
    if(in.size() < 8) throw std::runtime_error("Delta: too short");
    uint64_t sz = 0;
    for(int i=0;i<8;++i) sz |= (uint64_t)in[i]<<(i*8);
    std::vector<uint8_t> out; out.reserve(sz);
    uint8_t cur = 0;
    for(size_t i=8;i<in.size();++i){ cur += in[i]; out.push_back(cur); }
    out.resize(sz);
    return out;
}

} // namespace delta

// ============================================================
// RLE — ランレングス符号化
// ============================================================
// フォーマット: [orig_size:8LE] [run...]
//   run: もし先頭ビット=1 → [1|count-1: 7bit][value]  (繰り返し)
//        先頭ビット=0 → [0|count-1: 7bit][literals...]  (生データ)
namespace rle {

static std::vector<uint8_t> encode(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    out.reserve(8 + in.size());
    uint64_t sz = in.size();
    for(int i=0;i<8;++i) out.push_back((uint8_t)((sz>>(i*8))&0xFF));

    size_t pos = 0;
    const size_t n = in.size();
    while(pos < n){
        // ランのカウント
        size_t run = 1;
        while(pos+run < n && run < 128 && in[pos+run]==in[pos]) ++run;

        if(run > 1){
            out.push_back((uint8_t)(0x80 | (run - 1)));
            out.push_back(in[pos]);
            pos += run;
        } else {
            // リテラル列を溜める
            size_t lit = 0;
            size_t peek = pos;
            while(peek < n && lit < 128){
                size_t r = 1;
                while(peek+r < n && r < 128 && in[peek+r]==in[peek]) ++r;
                if(r >= 3) break; // ラン開始
                ++peek; ++lit;
            }
            if(lit == 0) lit = 1;
            out.push_back((uint8_t)(lit - 1));
            for(size_t k=0;k<lit;++k) out.push_back(in[pos+k]);
            pos += lit;
        }
    }
    return out;
}

static std::vector<uint8_t> decode(const std::vector<uint8_t>& in)
{
    if(in.size() < 8) throw std::runtime_error("RLE: too short");
    uint64_t sz = 0;
    for(int i=0;i<8;++i) sz |= (uint64_t)in[i]<<(i*8);
    std::vector<uint8_t> out; out.reserve(sz);
    size_t pos = 8;
    while(pos < in.size()){
        uint8_t hdr = in[pos++];
        if(hdr & 0x80){
            int count = (hdr & 0x7F) + 1;
            uint8_t val = in[pos++];
            for(int k=0;k<count;++k) out.push_back(val);
        } else {
            int count = (hdr & 0x7F) + 1;
            for(int k=0;k<count;++k) out.push_back(in[pos++]);
        }
    }
    out.resize(sz);
    return out;
}

} // namespace rle

// ============================================================
// 自動選択 — エントロピー + 構造解析
// ============================================================

// ============================================================
// Windows Compression API (MSZIP/XPRESS/XPRESS_HUFF/LZMS)
// Windows 8+ 標準搭載、cabinet.lib リンク必要
// ============================================================
#ifdef _WIN32
#include <windows.h>
#include <compressapi.h>
#pragma comment(lib,"cabinet.lib")

static std::vector<uint8_t> wincomp_compress(
    const std::vector<uint8_t>& in, DWORD algo)
{
    COMPRESSOR_HANDLE hc = nullptr;
    if(!CreateCompressor(algo, nullptr, &hc)) return in;

    SIZE_T outSz = 0;
    Compress(hc, in.data(), in.size(), nullptr, 0, &outSz);
    if(outSz == 0 || outSz >= in.size()){
        CloseCompressor(hc); return in; // 圧縮効果なし → パススルー
    }

    std::vector<uint8_t> out(outSz);
    if(!Compress(hc, in.data(), in.size(), out.data(), outSz, &outSz)){
        CloseCompressor(hc); return in;
    }
    out.resize(outSz);
    CloseCompressor(hc);

    // ヘッダ追加: [元サイズ:8B][圧縮データ]
    std::vector<uint8_t> result;
    uint64_t origSz = in.size();
    for(int i=0;i<8;i++) result.push_back((origSz>>(i*8))&0xFF);
    result.insert(result.end(), out.begin(), out.end());
    return result;
}

static std::vector<uint8_t> wincomp_decompress(
    const std::vector<uint8_t>& in, DWORD algo)
{
    if(in.size() < 8) return in;
    // 元サイズ読み取り
    uint64_t origSz = 0;
    for(int i=0;i<8;i++) origSz |= ((uint64_t)in[i])<<(i*8);
    const uint8_t* cdata = in.data() + 8;
    size_t csz = in.size() - 8;

    DECOMPRESSOR_HANDLE hd = nullptr;
    if(!CreateDecompressor(algo, nullptr, &hd)) return in;

    std::vector<uint8_t> out(origSz);
    SIZE_T actualSz = origSz;
    bool ok = !!Decompress(hd, cdata, csz, out.data(), origSz, &actualSz);
    CloseDecompressor(hd);
    if(!ok) return in;
    out.resize(actualSz);
    return out;
}
#else
// 非Windows: パススルー
static std::vector<uint8_t> wincomp_compress(const std::vector<uint8_t>& in, unsigned){ return in; }
static std::vector<uint8_t> wincomp_decompress(const std::vector<uint8_t>& in, unsigned){ return in; }
#define COMPRESS_ALGORITHM_MSZIP       2
#define COMPRESS_ALGORITHM_XPRESS      3
#define COMPRESS_ALGORITHM_XPRESS_HUFF 4
#define COMPRESS_ALGORITHM_LZMS        5
#endif

CompressMethod cpb_auto_detect(const std::vector<uint8_t>& data)
{
    if(data.empty()) return CompressMethod::NONE;
    size_t sample = std::min(data.size(), size_t(8192));

    // 1. バイト頻度ヒストグラム
    std::array<uint32_t,256> freq{};
    for(size_t i=0;i<sample;++i) freq[data[i]]++;

    // 2. Shannon エントロピー (bits/byte)
    double entropy = 0.0;
    for(auto f : freq){
        if(f == 0) continue;
        double p = (double)f / sample;
        entropy -= p * std::log2(p);
    }

    // 3. ランレングス比 (繰り返し率)
    size_t runs = 0;
    for(size_t i=1;i<sample;++i)
        if(data[i]==data[i-1]) ++runs;
    double run_ratio = (double)runs / sample;

    // 4. デルタ一貫性 (差分の標準偏差が小さい = 時系列)
    double delta_var = 0.0;
    if(sample >= 2){
        double mean = 0;
        for(size_t i=1;i<sample;++i)
            mean += (int8_t)(data[i] - data[i-1]);
        mean /= (sample - 1);
        for(size_t i=1;i<sample;++i){
            double d = (int8_t)(data[i]-data[i-1]) - mean;
            delta_var += d*d;
        }
        delta_var /= (sample - 1);
    }

    // 判定ロジック
    if(run_ratio > 0.70)
        return CompressMethod::RLE;      // 70%以上が繰り返し → RLE
    if(delta_var < 25.0 && entropy < 4.0)
        return CompressMethod::DELTA;    // デルタが小さく低エントロピー → Delta
    if(entropy < 5.5)
        return CompressMethod::ZSTD;     // 構造あり → Zstd
    if(entropy > 7.5)
        return CompressMethod::LZ4;      // ほぼランダム → LZ4(高速パススルー的)
    // Windows環境ではLZMSで最高圧縮を試みる
#ifdef _WIN32
    return CompressMethod::LZMS;
#else
    return CompressMethod::ZSTD;
#endif
}
#ifdef CPB_NO_ZSTD
// CPB_NO_ZSTD時のZSTDスタブ: LZ4でフォールバック圧縮
static std::vector<uint8_t> zstd_compress_impl(
    const std::vector<uint8_t>& in, int level){
    (void)level;
    return lz4::compress(in);
}
static std::vector<uint8_t> zstd_decompress_impl(
    const std::vector<uint8_t>& in, size_t hint){
    return lz4::decompress(in, hint);
}
#endif


// ============================================================
// 公開API
// ============================================================
std::vector<uint8_t> cpb_compress(
    const std::vector<uint8_t>& in,
    CompressMethod method,
    int level)
{
    CompressMethod m = (method == CompressMethod::AUTO)
                       ? cpb_auto_detect(in)
                       : method;

    // 先頭1バイトに実際に使用したmethodを記録
    std::vector<uint8_t> out;
    out.push_back((uint8_t)m);

    std::vector<uint8_t> compressed;
    switch(m){
    case CompressMethod::NONE:
        compressed = in;
        break;
    case CompressMethod::ZSTD:
        compressed = zstd_compress_impl(in, level);
        break;
    case CompressMethod::LZ4:
        compressed = lz4::compress(in);
        break;
    case CompressMethod::DELTA:
        compressed = delta::encode(in);
        break;
    case CompressMethod::RLE:
        compressed = rle::encode(in);
        break;
    default:
        compressed = zstd_compress_impl(in, level);
        break;
    }
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

std::vector<uint8_t> cpb_decompress(
    const std::vector<uint8_t>& in,
    CompressMethod method,  // AUTO時は先頭バイトから読む
    size_t orig_hint)
{
    if(in.empty()) return {};
    // 先頭バイトが実際のmethod
    CompressMethod m = (method == CompressMethod::AUTO)
                       ? (CompressMethod)in[0]
                       : method;
    const std::vector<uint8_t> payload(in.begin()+1, in.end());

    switch(m){
    case CompressMethod::NONE:
        return payload;
    case CompressMethod::ZSTD:
        return zstd_decompress_impl(payload, orig_hint);
    case CompressMethod::LZ4:
        return lz4::decompress(payload, orig_hint);
    case CompressMethod::DELTA:
        return delta::decode(payload);
    case CompressMethod::RLE:
        return rle::decode(payload);
    default:
        return zstd_decompress_impl(payload, orig_hint);
    }
}
