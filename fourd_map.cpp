#ifndef FOURD_MAP_CPP_INCLUDED
#define FOURD_MAP_CPP_INCLUDED
// fourd_map.cpp — CPB Level 4 4Dデータマッピング実装
#include "fourd_map.hpp"
#include "cpb_helpers.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <unordered_set>

// ============================================================
// xoshiro256** PRNG — 高品質疑似乱数 (シード再現性あり)
// ============================================================
struct Xoshiro256 {
    uint64_t s[4];

    explicit Xoshiro256(uint64_t seed) {
        // SplitMix64 でシードを展開
        auto splitmix = [](uint64_t& x) -> uint64_t {
            x += 0x9e3779b97f4a7c15ULL;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        };
        uint64_t x = seed;
        s[0]=splitmix(x); s[1]=splitmix(x);
        s[2]=splitmix(x); s[3]=splitmix(x);
    }

    uint64_t next() {
        auto rotl=[](uint64_t x,int k){return(x<<k)|(x>>(64-k));};
        uint64_t r = rotl(s[1]*5,7)*9;
        uint64_t t = s[1]<<17;
        s[2]^=s[0]; s[3]^=s[1]; s[1]^=s[2]; s[0]^=s[3];
        s[2]^=t; s[3]=rotl(s[3],45);
        return r;
    }

    // [0, n) の一様乱数
    uint64_t next_range(uint64_t n) {
        if(n==0) return 0;
        // rejection sampling で偏りなし
        uint64_t threshold = (~uint64_t(0) % n + 1) % n;
        uint64_t r;
        do { r = next(); } while(r < threshold);
        return r % n;
    }
};

// ============================================================
// Fisher-Yates シャッフル (インデックス列を生成)
// セル数が多い場合は全列挙せずにオンデマンド生成する
// ============================================================

// Feistel ネットワーク: [0, N) のランダム順列を O(1)/element で生成
// シード + インデックス → 別のインデックス (双射)
static uint64_t feistel_permute(uint64_t idx, uint64_t N, uint64_t seed)
{
    if(N == 0) return 0;
    uint64_t half = 1;
    while(half * half < N) ++half; // ceil(sqrt(N))

    // cycle-walking をイテレーティブに (再帰スタックオーバーフロー防止)
    uint64_t attempt = idx;
    for(int guard = 0; guard < 256; ++guard) {
        uint64_t L = attempt % half;
        uint64_t R = attempt / half;
        for(int r=0;r<4;++r){
            uint64_t f = (R ^ seed) * 0x9e3779b97f4a7c15ULL;
            f ^= (f >> 31) + seed * (uint64_t)(r+1);
            f ^= attempt * 0x6c62272e07bb0142ULL; // attempt依存で毎回変化
            uint64_t newL = R;
            uint64_t newR = L ^ (f % half);
            L=newL; R=newR;
        }
        uint64_t result = R*half + L;
        if(result < N) return result;
        attempt = result;
        seed ^= 0x9e3779b97f4a7c15ULL; // 毎ループseedを更新
    }
    return idx % N; // フォールバック
}

// ============================================================
// 4Dアドレス生成
// ============================================================
// 全セルをフラットインデックスで管理:
//   [0 .. video_cells-1]   → 動画ピクセル
//   [video_cells .. total-1] → 音声サンプル

static CellAddress flat_to_cell(uint64_t flat_idx,
                                 const FourDSpace& space)
{
    CellAddress addr;
    uint64_t vcells = space.video_cells();

    if(flat_idx < vcells) {
        // 動画ピクセル: flat = z*W*H + y*W + x
        uint64_t rem = flat_idx;
        addr.x = (uint16_t)(rem % space.width);  rem /= space.width;
        addr.y = (uint16_t)(rem % space.height);  rem /= space.height;
        addr.z = (uint32_t)rem;
        addr.w = UINT32_MAX; // 音声なし
    } else {
        // 音声サンプル
        uint64_t aidx = flat_idx - vcells;
        addr.x = UINT16_MAX; // 動画なし
        addr.y = UINT16_MAX;
        addr.z = UINT32_MAX;
        addr.w = (uint32_t)aidx;
    }
    return addr;
}

// ============================================================
// マッピング本体
// ============================================================
FourDMapping map_payload(
    const std::vector<uint8_t>& payload,
    const FourDSpace& space,
    uint64_t seed,
    uint8_t bits_per_cell)
{
    if(bits_per_cell == 0 || bits_per_cell > 8)
        throw std::runtime_error("4D: bits_per_cell must be 1..8");

    uint64_t total = space.total_cells();
    if(total == 0) throw std::runtime_error("4D: empty space");

    uint64_t bits_needed  = (uint64_t)payload.size() * 8;
    uint64_t cells_needed = (bits_needed + bits_per_cell - 1) / bits_per_cell;

    if(cells_needed > total)
        throw std::runtime_error("4D: payload too large for this space");

    FourDMapping mapping;
    mapping.space         = space;
    mapping.seed          = seed;
    mapping.bits_per_cell = bits_per_cell;
    mapping.addresses.reserve(cells_needed);

    // Xoshiro256 PRNG + 重複排除で衝突なしのランダムセル列を生成
    // cells_needed << total のケースでは衝突率が低く高速
    // cells_needed が total に近い場合でも unordered_set で保証
    Xoshiro256 rng(seed);
    std::unordered_set<uint64_t> used;
    used.reserve(cells_needed * 2);

    for(uint64_t i = 0; i < cells_needed; ) {
        uint64_t flat = rng.next() % total;
        if(used.count(flat)) continue; // 衝突 → 次のランダム値を試す
        used.insert(flat);
        mapping.addresses.push_back(flat_to_cell(flat, space));
        ++i;
    }

    return mapping;
}

// ============================================================
// アンマッピング (アドレス情報からペイロード復元)
// ============================================================
std::vector<uint8_t> unmap_payload(const FourDMapping& mapping)
{
    // アドレス列から元のフラットインデックスを逆算
    // → FourDSpace + seed があれば再生成できる
    // ここでは mapping.addresses のインデックス順序から復元

    if(mapping.addresses.empty()) return {};

    uint64_t total_bits = (uint64_t)mapping.addresses.size()
                        * mapping.bits_per_cell;
    size_t byte_count = total_bits / 8;
    std::vector<uint8_t> payload(byte_count, 0);

    // bit_idx → byte/bit位置
    uint64_t bit_idx = 0;
    for(size_t cell = 0; cell < mapping.addresses.size(); ++cell) {
        for(int b = 0; b < mapping.bits_per_cell; ++b, ++bit_idx) {
            if(bit_idx >= total_bits) break;
            size_t byte_pos = bit_idx / 8;
            int    bit_pos  = 7 - (int)(bit_idx % 8);
            if(byte_pos < payload.size()) {
                // アドレスのhash下位ビットを"仮想ピクセル値"として使用
                // 実際の動画フレームデータから読む場合はここを置き換え
                uint64_t fake_val = (uint64_t)mapping.addresses[cell].x
                                  ^ (uint64_t)mapping.addresses[cell].y * 31
                                  ^ (uint64_t)mapping.addresses[cell].z * 1337;
                uint8_t bit = (fake_val >> b) & 1;
                payload[byte_pos] |= (bit << bit_pos);
            }
        }
    }
    return payload;
}

// ============================================================
// 統計
// ============================================================
MappingStats calc_stats(const FourDMapping& m)
{
    MappingStats s;
    s.total_cells = m.space.total_cells();
    s.used_cells  = m.addresses.size();
    s.density     = s.total_cells > 0
                  ? (double)s.used_cells / s.total_cells
                  : 0.0;

    uint64_t vcells = m.space.video_cells();
    uint64_t acells = m.space.audio_samples;
    uint64_t video_used = 0, audio_used = 0;

    for(auto& a : m.addresses) {
        if(a.z != UINT32_MAX) ++video_used;
        else                  ++audio_used;
    }

    s.video_usage_pct = vcells > 0
                      ? (double)video_used / vcells * 100.0 : 0.0;
    s.audio_usage_pct = acells > 0
                      ? (double)audio_used / acells * 100.0 : 0.0;

    // 探索空間: total_cells の順列数
    // シードが64bitなので実質 2^64 だが
    // 「どの total_cells 個のセルを使うか」の組合せ数で表現
    // C(total, used) が真の探索空間だが天文学的なので
    // total_cells をそのまま keyspace として返す
    s.key_space   = s.total_cells;
    s.active_dims = 4 + m.space.dims.size(); // 基本4次元 + 拡張

    return s;
}

// ============================================================
// シリアライズ
// ============================================================

// フォーマット: magic[4]="4DMP" + version(1) + bits_per_cell(1)
//              seed(8) + space(width2+height2+frames4+audio4+rate4+ch1)
//              addr_count(8) + [x2+y2+z4+w4]*N
std::vector<uint8_t> serialize_mapping(const FourDMapping& m)
{
    std::vector<uint8_t> out;
    out.reserve(20 + 12 * m.addresses.size());

    // magic
    for(char c : {'4','D','M','P'}) out.push_back((uint8_t)c);
    w8(out,1);                          // version
    w8(out,m.bits_per_cell);
    w64(out,m.seed);
    // space
    w16(out,m.space.width);
    w16(out,m.space.height);
    w32(out,m.space.frames);
    w32(out,m.space.audio_samples);
    w32(out,m.space.audio_rate);
    w8(out,m.space.channels);
    // 拡張次元 (DimExtension列)
    w8(out,(uint8_t)m.space.dims.size());
    for(auto& d : m.space.dims){
        w8(out,(uint8_t)d.id);
        w32(out,d.size);
        w8(out,(uint8_t)d.note.size());
        out.insert(out.end(),d.note.begin(),d.note.end());
    }
    // addresses
    w64(out,(uint64_t)m.addresses.size());
    for(auto& a : m.addresses){
        w16(out,a.x); w16(out,a.y);
        w32(out,a.z); w32(out,a.w);
        // 拡張次元の値
        w8(out,(uint8_t)a.ext.size());
        for(auto& [k,v] : a.ext){ w8(out,k); w32(out,v); }
    }
    return out;
}

FourDMapping deserialize_mapping(const std::vector<uint8_t>& buf)
{
    if(buf.size() < 4) throw std::runtime_error("4D: buffer too short");
    const uint8_t* p = buf.data();
    if(memcmp(p,"4DMP",4)!=0) throw std::runtime_error("4D: bad magic");
    p+=4;

    FourDMapping m;
    r8(p); // version
    m.bits_per_cell = r8(p);
    m.seed          = r64(p);
    m.space.width         = r16(p);
    m.space.height        = r16(p);
    m.space.frames        = r32(p);
    m.space.audio_samples = r32(p);
    m.space.audio_rate    = r32(p);
    m.space.channels      = r8(p);
    // 拡張次元
    uint8_t nd = r8(p);
    for(uint8_t i=0;i<nd;++i){
        DimExtension d;
        d.id   = (DimID)r8(p);
        d.size = r32(p);
        uint8_t nlen = r8(p);
        d.note = std::string((const char*)p, nlen); p+=nlen;
        m.space.dims.push_back(d);
    }

    uint64_t n = r64(p);
    m.addresses.reserve(n);
    for(uint64_t i=0;i<n;++i){
        CellAddress a;
        a.x=r16(p); a.y=r16(p);
        a.z=r32(p); a.w=r32(p);
        uint8_t ne = r8(p);
        for(uint8_t j=0;j<ne;++j){
            uint8_t k=r8(p); uint32_t v=r32(p); a.ext[k]=v;
        }
        m.addresses.push_back(a);
    }
    return m;
}

// ============================================================
// ユーティリティ
// ============================================================
PixelPos cell_to_pixel(const CellAddress& addr)
{
    return {addr.x, addr.y, addr.z};
}

AudioPos cell_to_audio(const CellAddress& addr, uint8_t channels)
{
    uint8_t ch = channels > 0 ? (uint8_t)(addr.w % channels) : 0;
    return {addr.w / (channels > 0 ? channels : 1), ch};
}

std::string keyspace_description(const FourDSpace& space)
{
    uint64_t total = space.total_cells();
    std::ostringstream oss;
    oss << "探索空間: " << total << " cells\n";

    // log2
    double bits = std::log2((double)total);
    oss << std::fixed << std::setprecision(1);
    oss << "  = 2^" << bits << " (約 " << (int)bits << " bit強度)\n";
    oss << "  内訳: 映像 "
        << space.width << "x" << space.height << "x" << space.frames
        << "f + 音声 " << space.audio_samples << " samples\n";
    oss << "  シード64bit → " << (uint64_t(1)<<32) << "^2 通り";
    return oss.str();
}

// ============================================================
// unmap_payload_from_frames — 実フレームデータからの復元 (#7修正)
// ============================================================
std::vector<uint8_t> unmap_payload_from_frames(
    const FourDMapping& mapping,
    const PixelReader& pixel_reader,
    const AudioReader& audio_reader)
{
    if(mapping.addresses.empty()) return {};

    uint64_t total_bits = (uint64_t)mapping.addresses.size()
                        * mapping.bits_per_cell;
    size_t byte_count = total_bits / 8;
    std::vector<uint8_t> payload(byte_count, 0);

    uint64_t bit_idx = 0;
    for(size_t cell = 0; cell < mapping.addresses.size(); ++cell) {
        const auto& addr = mapping.addresses[cell];
        for(int b = 0; b < mapping.bits_per_cell; ++b, ++bit_idx) {
            if(bit_idx >= total_bits) break;
            size_t byte_pos = bit_idx / 8;
            int    bit_pos  = 7 - (int)(bit_idx % 8);
            if(byte_pos >= payload.size()) break;

            uint8_t pixel_val = 0;
            if(addr.z != UINT32_MAX && pixel_reader) {
                // 動画フレームからピクセル値を読む
                pixel_val = pixel_reader(addr.z, addr.x, addr.y);
            } else if(addr.w != UINT32_MAX && audio_reader) {
                // 音声サンプルから読む
                pixel_val = audio_reader(addr.w);
            }
            uint8_t bit = (pixel_val >> b) & 1;
            payload[byte_pos] |= (uint8_t)(bit << bit_pos);
        }
    }
    return payload;
}

// ============================================================
// map_payload_to_frames — 実フレームデータへの書き込み
// ============================================================
void map_payload_to_frames(
    const FourDMapping& mapping,
    const std::vector<uint8_t>& payload,
    const PixelWriter& pixel_writer,
    const AudioWriter& audio_writer)
{
    if(mapping.addresses.empty() || payload.empty()) return;

    uint64_t total_bits = (uint64_t)mapping.addresses.size()
                        * mapping.bits_per_cell;
    uint64_t payload_bits = (uint64_t)payload.size() * 8;

    uint64_t bit_idx = 0;
    for(size_t cell = 0; cell < mapping.addresses.size(); ++cell) {
        const auto& addr = mapping.addresses[cell];
        for(int b = 0; b < mapping.bits_per_cell; ++b, ++bit_idx) {
            if(bit_idx >= total_bits || bit_idx >= payload_bits) break;
            size_t byte_pos = bit_idx / 8;
            int    bit_pos  = 7 - (int)(bit_idx % 8);
            uint8_t bit = (payload[byte_pos] >> bit_pos) & 1;

            if(addr.z != UINT32_MAX && pixel_writer) {
                // ピクセルの下位bitにデータを埋め込む (LSB steganography)
                // 実際のピクセル値を読んでLSBを変えて書き戻す
                pixel_writer(addr.z, addr.x, addr.y, bit);
            } else if(addr.w != UINT32_MAX && audio_writer) {
                audio_writer(addr.w, bit);
            }
        }
    }
}

#endif // FOURD_MAP_CPP_INCLUDED
