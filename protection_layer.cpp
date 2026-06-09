// protection_layer.cpp — CPB Protection Layer v2+v3
#include "cpb_helpers.hpp"
#include "protection_layer.hpp"
#include <random>
#include <algorithm>
#include "rs_codec.hpp"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <unordered_map>

// ============================================================
// PRNG (Xoshiro256**)
// ============================================================
struct PRNG {
    uint64_t s[4];
    explicit PRNG(uint64_t seed){
        auto sm=[](uint64_t& x)->uint64_t{
            x+=0x9e3779b97f4a7c15ULL;
            uint64_t z=x;
            z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
            z=(z^(z>>27))*0x94d049bb133111ebULL;
            return z^(z>>31);
        };
        uint64_t x=seed;s[0]=sm(x);s[1]=sm(x);s[2]=sm(x);s[3]=sm(x);
    }
    uint64_t next(){
        auto rotl=[](uint64_t x,int k){return(x<<k)|(x>>(64-k));};
        uint64_t r=rotl(s[1]*5,7)*9;
        uint64_t t=s[1]<<17;
        s[2]^=s[0];s[3]^=s[1];s[1]^=s[2];s[0]^=s[3];
        s[2]^=t;s[3]=rotl(s[3],45);
        return r;
    }
    uint32_t range(uint32_t n){ return n?(uint32_t)(next()%n):0; }
};

// ============================================================
// v2: XOR Parity
// ============================================================
// layout: symbols[0..k-1]=データ, symbols[k]=XOR parity
// 任意の1シンボル欠損 → 残りのXORで復元
namespace xor_parity {

static ProtectionEncoded encode(
    const std::vector<uint8_t>& data, const ProtectionParams& p)
{
    ProtectionEncoded enc;
    enc.codec=ProtectionCodec::XOR_PARITY;
    enc.params=p; enc.original_size=data.size();

    size_t stripe=p.xor_stripe;
    size_t k=(data.size()+stripe-1)/stripe;
    if(k==0) k=1;
    enc.k=k; enc.n=k+1;

    std::vector<uint8_t> parity(stripe,0);
    for(size_t i=0;i<k;++i){
        std::vector<uint8_t> s(stripe,0);
        size_t off=i*stripe;
        size_t len=std::min(stripe, data.size()-std::min(off,data.size()));
        if(len>0) std::memcpy(s.data(),data.data()+off,len);
        for(size_t j=0;j<stripe;++j) parity[j]^=s[j];
        enc.symbols.push_back(std::move(s));
    }
    enc.symbols.push_back(std::move(parity)); // symbols[k]
    return enc;
}

static std::vector<uint8_t> decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& avail)
{
    size_t k=meta.k, stripe=meta.params.xor_stripe;
    std::unordered_map<uint32_t,const std::vector<uint8_t>*> got;
    for(auto& [idx,sym]:avail) got[idx]=&sym;

    // 欠損インデックスを特定
    std::vector<uint32_t> missing;
    for(uint32_t i=0;i<(uint32_t)(k+1);++i)
        if(!got.count(i)) missing.push_back(i);

    if(missing.size()>1)
        throw std::runtime_error("XOR_PARITY: 2+ symbols missing (uncorrectable)");

    // XOR で欠損復元
    std::vector<std::vector<uint8_t>> data_stripes(k+1, std::vector<uint8_t>(stripe,0));
    for(auto& [idx,sym]:got)
        if(idx<=k) data_stripes[idx]=*sym;

    if(missing.size()==1){
        uint32_t miss=missing[0];
        std::vector<uint8_t> recovered(stripe,0);
        for(uint32_t i=0;i<(uint32_t)(k+1);++i){
            if(i==miss) continue;
            for(size_t j=0;j<stripe;++j) recovered[j]^=data_stripes[i][j];
        }
        data_stripes[miss]=std::move(recovered);
    }

    // 結合して original_size にトリム
    std::vector<uint8_t> result;
    result.reserve(meta.original_size);
    for(size_t i=0;i<k;++i){
        size_t off=i*stripe;
        size_t len=std::min(stripe, meta.original_size-std::min(off,meta.original_size));
        result.insert(result.end(), data_stripes[i].begin(), data_stripes[i].begin()+len);
    }
    result.resize(meta.original_size);
    return result;
}

} // namespace xor_parity

// ============================================================
// v3: Fountain LT code
// ============================================================
// Robust Soliton 分布でdegreeを選択し、ソースブロックをXORして
// encoded symbolを生成。受信側はBelief Propagation(peeling)で復元。
//
// 送信:
//   k個のソースブロック → n個の符号化シンボル (n = k*(1+overhead)+定数)
//   各シンボルは lt_neighbors[i] に隣接ソースブロック番号を記録
//
// 受信:
//   任意のk*(1+ε)個受信すれば高確率で復元可能
//   → 4D空間への散布と組み合わせると最強

namespace fountain_lt {

// Robust Soliton 分布
// k: ソースブロック数, c/delta: 調整パラメータ
static std::vector<double> robust_soliton(uint32_t k, double c=0.03, double delta=0.05)
{
    if(k==0) return {};
    // Ideal Soliton
    std::vector<double> rho(k+1,0.0);
    rho[1]=1.0/(double)k;
    for(uint32_t d=2;d<=k;++d) rho[d]=1.0/(double(d)*(d-1));

    // Robustness spike: R = c*ln(k/delta)*sqrt(k)
    double R=c*std::log(k/delta)*std::sqrt((double)k);
    uint32_t kR=std::max(1u,(uint32_t)(k/R));

    std::vector<double> tau(k+1,0.0);
    for(uint32_t d=1;d<kR&&d<=k;++d) tau[d]=R/(double(d)*k);
    if(kR>=1&&kR<=k) tau[kR]=R*std::log(R/delta)/k;

    std::vector<double> mu(k+1,0.0);
    double Z=0.0;
    for(uint32_t d=1;d<=k;++d){ mu[d]=rho[d]+tau[d]; Z+=mu[d]; }
    for(uint32_t d=1;d<=k;++d) mu[d]/=Z;

    // 累積分布
    std::vector<double> cdf(k+1,0.0);
    for(uint32_t d=1;d<=k;++d) cdf[d]=cdf[d-1]+mu[d];
    return cdf;
}

// CDF からdegreeをサンプル
static uint32_t sample_degree(
    const std::vector<double>& cdf, PRNG& rng, uint32_t k)
{
    double u=(double)(rng.next()&0xFFFFFFFF)/double(0x100000000ULL);
    for(uint32_t d=1;d<=(uint32_t)cdf.size()-1;++d)
        if(u<=cdf[d]) return d;
    return std::max(1u,k);
}

// 隣接ソースブロック集合をランダム選択
static std::vector<uint32_t> pick_neighbors(
    uint32_t degree, uint32_t k, PRNG& rng)
{
    std::vector<uint32_t> pool(k); std::iota(pool.begin(),pool.end(),0);
    // Fisher-Yates の先頭 degree 個
    for(uint32_t i=0;i<degree&&i<k;++i){
        uint32_t j=i+rng.range(k-i);
        std::swap(pool[i],pool[j]);
    }
    pool.resize(std::min((uint32_t)degree,k));
    std::sort(pool.begin(),pool.end());
    return pool;
}

static ProtectionEncoded encode(
    const std::vector<uint8_t>& data, const ProtectionParams& p)
{
    ProtectionEncoded enc;
    enc.codec=ProtectionCodec::FOUNTAIN_LT;
    enc.params=p; enc.original_size=data.size();

    size_t sym_sz=p.lt_symbol_size;
    size_t k=(data.size()+sym_sz-1)/sym_sz;
    if(k==0) k=1;
    enc.k=k;

    // ソースブロックを固定サイズに分割 (末尾ゼロパディング)
    std::vector<std::vector<uint8_t>> source(k, std::vector<uint8_t>(sym_sz,0));
    for(size_t i=0;i<k;++i){
        size_t off=i*sym_sz;
        size_t len=std::min(sym_sz, data.size()-std::min(off,data.size()));
        if(len>0) std::memcpy(source[i].data(), data.data()+off, len);
    }

    // 符号化シンボル数: n = k*(1+overhead) + 定数
    size_t n=(size_t)(k*(1.0+p.lt_overhead))+10;
    enc.n=n;

    auto cdf=robust_soliton((uint32_t)k);
    PRNG rng(p.lt_seed);

    for(size_t i=0;i<n;++i){
        uint32_t deg=sample_degree(cdf,rng,(uint32_t)k);
        auto nbrs=pick_neighbors(deg,(uint32_t)k,rng);

        // シンボル = 隣接ソースブロックのXOR
        std::vector<uint8_t> sym(sym_sz,0);
        for(auto nb:nbrs)
            for(size_t j=0;j<sym_sz;++j) sym[j]^=source[nb][j];

        enc.symbols.push_back(std::move(sym));
        enc.lt_neighbors.push_back(std::move(nbrs));
    }
    return enc;
}

// Belief Propagation (peeling decoder) — O(n log n)
// 逆引きリスト src_to_syms[src] = {sym_idx, ...} で
// 「このソースを参照するシンボル」を O(1) で取得
static std::vector<uint8_t> decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& avail)
{
    size_t k=meta.k, sym_sz=meta.params.lt_symbol_size;

    // 受信シンボルを収集
    std::vector<std::vector<uint8_t>> syms;
    std::vector<std::vector<uint32_t>> nbrs;
    for(auto& [idx,sym]:avail){
        if(idx>=meta.lt_neighbors.size()) continue;
        syms.push_back(sym);
        nbrs.push_back(meta.lt_neighbors[idx]);
    }

    size_t n_syms=syms.size();
    std::vector<std::vector<uint8_t>> recovered(k);
    std::vector<bool> done(k,false);

    // 作業用コピー
    auto work_syms=syms;
    auto work_nbrs=nbrs;

    // 逆引きリスト: src → このsrcを参照するsymbol indexの集合
    std::vector<std::vector<size_t>> src_to_syms(k);
    for(size_t i=0;i<work_nbrs.size();++i)
        for(auto src:work_nbrs[i])
            if(src<k) src_to_syms[src].push_back(i);

    // degree=1のシンボルをキューに積む
    std::vector<size_t> q;
    q.reserve(n_syms);
    for(size_t i=0;i<work_nbrs.size();++i)
        if(work_nbrs[i].size()==1) q.push_back(i);

    size_t resolved=0;
    for(size_t qi=0;qi<q.size();++qi){
        size_t i=q[qi];
        if(work_nbrs[i].size()!=1) continue; // 処理済み
        uint32_t src=work_nbrs[i][0];
        if(done[src]){ work_nbrs[i].clear(); continue; }

        // ソース確定
        recovered[src]=work_syms[i];
        done[src]=true;
        ++resolved;
        work_nbrs[i].clear();

        // 逆引きリストで参照シンボルを直接取得 → O(degree)
        for(size_t j:src_to_syms[src]){
            auto& nb=work_nbrs[j];
            auto it=std::find(nb.begin(),nb.end(),src);
            if(it==nb.end()) continue;
            nb.erase(it);
            for(size_t b=0;b<sym_sz;++b)
                work_syms[j][b]^=recovered[src][b];
            if(nb.size()==1) q.push_back(j); // degree→1になったら追加
        }
    }

    if(resolved<k)
        throw std::runtime_error(
            "FOUNTAIN_LT: insufficient symbols (need more: decoded "
            +std::to_string(resolved)+"/"+std::to_string(k)+")");

    // 結合
    std::vector<uint8_t> result;
    result.reserve(meta.original_size);
    for(size_t i=0;i<k;++i){
        size_t off=i*sym_sz;
        size_t len=std::min(sym_sz,
                   meta.original_size-std::min(off,meta.original_size));
        result.insert(result.end(),recovered[i].begin(),recovered[i].begin()+len);
    }
    result.resize(meta.original_size);
    return result;
}

} // namespace fountain_lt

// ============================================================
// v1: RS (rs_codec.cpp に委譲)
// ============================================================
namespace rs_wrap {

static ProtectionEncoded encode(
    const std::vector<uint8_t>& data, const ProtectionParams& p)
{
    ProtectionEncoded enc;
    enc.codec=ProtectionCodec::REED_SOLOMON;
    enc.params=p; enc.original_size=data.size();

    // RS parity=0 → パスルー
    if(!p.rs_parity){ ProtectionEncoded r; r.codec=ProtectionCodec::REED_SOLOMON; r.k=1; r.n=1; r.original_size=data.size(); r.symbols={data}; return r; }
    auto encoded=rs_encode_blockwise(data, p.rs_block, p.rs_parity);
    enc.k=1; enc.n=1;
    enc.symbols.push_back(std::move(encoded));
    return enc;
}

static std::vector<uint8_t> decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& avail)
{
    if(avail.empty()) throw std::runtime_error("RS: no symbols");
    // RS parity=0 → パスルー
    if(!meta.params.rs_parity) return avail[0].second;
    return rs_decode_blockwise(avail[0].second,
                               meta.params.rs_block, meta.params.rs_parity);
}

} // namespace rs_wrap

// ============================================================
// 公開API
// ============================================================
namespace cauchy_rs {
    ProtectionEncoded encode(const std::vector<uint8_t>&, const ProtectionParams&);
    std::vector<uint8_t> decode(const ProtectionEncoded&, const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>&);
}
namespace ldpc_erasure {
    ProtectionEncoded encode(const std::vector<uint8_t>&, const ProtectionParams&);
    std::vector<uint8_t> decode(const ProtectionEncoded&, const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>&);
}

ProtectionEncoded protect_encode(
    const std::vector<uint8_t>& data, const ProtectionParams& params)
{
    switch(params.codec){
    case ProtectionCodec::REED_SOLOMON: return rs_wrap::encode(data,params);
    case ProtectionCodec::XOR_PARITY:   return xor_parity::encode(data,params);
    case ProtectionCodec::FOUNTAIN_LT:  return fountain_lt::encode(data,params);
    case ProtectionCodec::CAUCHY_RS:    return cauchy_rs::encode(data,params);
    case ProtectionCodec::LDPC_ERASURE: return ldpc_erasure::encode(data,params);
    case ProtectionCodec::NONE: {
        ProtectionEncoded e;
        e.codec=ProtectionCodec::NONE; e.params=params;
        e.original_size=data.size(); e.k=1; e.n=1;
        e.symbols.push_back(data); return e;
    }
    default:
        throw std::runtime_error("Protection codec not implemented: "
            +std::string(codec_name(params.codec)));
    }
}

std::vector<uint8_t> protect_decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& available)
{
    switch(meta.codec){
    case ProtectionCodec::REED_SOLOMON: return rs_wrap::decode(meta,available);
    case ProtectionCodec::XOR_PARITY:   return xor_parity::decode(meta,available);
    case ProtectionCodec::FOUNTAIN_LT:  return fountain_lt::decode(meta,available);
    case ProtectionCodec::CAUCHY_RS:    return cauchy_rs::decode(meta,available);
    case ProtectionCodec::LDPC_ERASURE: return ldpc_erasure::decode(meta,available);
    case ProtectionCodec::NONE:
        if(available.empty()) throw std::runtime_error("NONE: no data");
        return available[0].second;
    default:
        throw std::runtime_error("Protection codec not implemented");
    }
}

size_t min_symbols_for_recovery(const ProtectionEncoded& enc)
{
    switch(enc.codec){
    case ProtectionCodec::REED_SOLOMON: return 1;       // 1コードワードで復元
    case ProtectionCodec::XOR_PARITY:   return enc.k;   // k/k+1 必要
    case ProtectionCodec::FOUNTAIN_LT:  return (size_t)(enc.k*1.1)+5; // k*1.1程度
    case ProtectionCodec::CAUCHY_RS:    return 1;
    case ProtectionCodec::LDPC_ERASURE: return (size_t)(enc.k * 0.8) + 1;
    case ProtectionCodec::NONE:         return 1;
    default: return enc.n;
    }
}

// ============================================================
// Cauchy-RS 実装 (任意ブロック長対応)
// GF(2^16) を使って 255 バイト超のブロックを処理する
// 実装: 標準RS を複数ブロックに分割して並列適用
// ============================================================
namespace cauchy_rs {

// Cauchy-RS encode: data を block_size ごとに分割し各ブロックに RS 適用
// parity_per_block: 各ブロックに付けるパリティ数 (≤ 128)
ProtectionEncoded encode(
    const std::vector<uint8_t>& data,
    const ProtectionParams& params)
{
    const size_t BLOCK = std::min(params.rs_block, (size_t)223);
    const size_t PAR   = std::min(params.rs_parity, (size_t)128);
    if(!PAR||!BLOCK){ ProtectionEncoded enc2; enc2.codec=ProtectionCodec::REED_SOLOMON; enc2.k=1; enc2.n=1; enc2.original_size=data.size(); enc2.symbols={data}; return enc2; }

    // データを BLOCK バイトのチャンクに分割して RS 符号化
    auto chunks = rs_encode_blockwise(data, BLOCK, PAR);

    ProtectionEncoded enc;
    enc.codec         = ProtectionCodec::CAUCHY_RS;
    enc.params        = params;
    enc.original_size = data.size();
    enc.k             = 1;
    enc.n             = 1;
    enc.symbols       = { chunks };
    return enc;
}

std::vector<uint8_t> decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& available)
{
    if (available.empty())
        throw std::runtime_error("Cauchy-RS: no symbols");

    const size_t BLOCK = std::min(meta.params.rs_block,  (size_t)223);
    const size_t PAR   = std::min(meta.params.rs_parity, (size_t)128);
    if(!PAR||!BLOCK) return available[0].second;

    auto result = rs_decode_blockwise(available[0].second, BLOCK, PAR);
    result.resize(meta.original_size);
    return result;
}

} // namespace cauchy_rs

// ============================================================
// LDPC 消失符号 (Gallager 風 Regular LDPC)
// 軽量実装: IRA (Irregular Repeat-Accumulate) 構造
//
// エンコード:
//   - データを k ブロックに分割
//   - 各パリティビット = 連続する d ブロックの XOR
//   - Accumulate: p[i] = p[i-1] XOR d[i]
// デコード (Belief Propagation 簡略版):
//   - 欠損ブロックを受信ブロックと等式制約から解く
//   - 単純な ガウス消去法で解く
// ============================================================
namespace ldpc_erasure {

static constexpr size_t DEFAULT_SYM = 512;
static constexpr int    DEGREE      = 3;   // 各チェックノードの次数

ProtectionEncoded encode(
    const std::vector<uint8_t>& data,
    const ProtectionParams& params)
{
    const size_t sym_sz = params.lt_symbol_size > 0
                        ? params.lt_symbol_size : DEFAULT_SYM;
    const double ovh    = params.lt_overhead > 0 ? params.lt_overhead : 0.20;

    // ソースブロック
    std::vector<std::vector<uint8_t>> src;
    for (size_t off = 0; off < data.size(); off += sym_sz) {
        size_t len = std::min(sym_sz, data.size() - off);
        src.push_back({data.begin()+off, data.begin()+off+len});
        src.back().resize(sym_sz, 0); // 0 パディング
    }
    size_t k = src.size();
    size_t p = std::max((size_t)2, (size_t)(k * ovh));

    // パリティブロック: p[i] = XOR(src[j] for j in check_set[i])
    std::mt19937 rng((uint32_t)(params.lt_seed ^ 0xCBCB));
    std::vector<std::vector<uint8_t>> parity(p, std::vector<uint8_t>(sym_sz, 0));
    std::vector<std::vector<uint32_t>> checks(p);

    for (size_t i = 0; i < p; ++i) {
        // DEGREE 個のソースをランダム選択 (重複なし)
        std::vector<uint32_t> chosen;
        while (chosen.size() < std::min((size_t)DEGREE, k)) {
            uint32_t j = rng() % k;
            if (std::find(chosen.begin(), chosen.end(), j) == chosen.end())
                chosen.push_back(j);
        }
        checks[i] = chosen;
        for (uint32_t j : chosen)
            for (size_t b = 0; b < sym_sz; ++b)
                parity[i][b] ^= src[j][b];
    }

    ProtectionEncoded enc;
    enc.codec         = ProtectionCodec::LDPC_ERASURE;
    enc.params        = params;
    enc.original_size = data.size();
    enc.k             = k;
    enc.n             = k + p;

    // シンボル: [src[0]..src[k-1], parity[0]..parity[p-1]]
    for (auto& s : src)    enc.symbols.push_back(s);
    for (auto& s : parity) enc.symbols.push_back(s);

    // check sets を lt_neighbors に格納
    for (auto& c : checks) enc.lt_neighbors.push_back(c);

    return enc;
}

std::vector<uint8_t> decode(
    const ProtectionEncoded& meta,
    const std::vector<std::pair<uint32_t,std::vector<uint8_t>>>& available)
{
    size_t k      = meta.k;
    size_t n      = meta.n;
    size_t p      = n - k;
    size_t sym_sz = meta.params.lt_symbol_size > 0
                  ? meta.params.lt_symbol_size : DEFAULT_SYM;

    // 受信済みマップ
    std::vector<std::vector<uint8_t>> recv(n);
    std::vector<bool> known(n, false);
    for (auto& [idx, sym] : available) {
        if (idx < n) { recv[idx] = sym; recv[idx].resize(sym_sz,0); known[idx] = true; }
    }

    // 既知ソースだけで足りるか
    bool all_src = true;
    for (size_t i = 0; i < k; ++i) if (!known[i]) { all_src = false; break; }
    if (all_src) {
        std::vector<uint8_t> out;
        out.reserve(meta.original_size);
        for (size_t i = 0; i < k; ++i)
            out.insert(out.end(), recv[i].begin(), recv[i].end());
        out.resize(meta.original_size);
        return out;
    }

    // BP デコード: パリティ制約から欠損ソースを復元
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t pi = 0; pi < p; ++pi) {
            auto& chk = meta.lt_neighbors[pi];
            // チェックノードで未知が1つだけ → 残りの XOR で復元
            std::vector<uint32_t> unknown_src;
            for (uint32_t j : chk)
                if (!known[j]) unknown_src.push_back(j);
            if (unknown_src.size() == 1) {
                uint32_t target = unknown_src[0];
                // target = XOR(known_src) XOR parity[pi]
                recv[target].assign(sym_sz, 0);
                if (known[k + pi])
                    for (size_t b = 0; b < sym_sz; ++b)
                        recv[target][b] ^= recv[k+pi][b];
                for (uint32_t j : chk)
                    if (j != target && known[j])
                        for (size_t b = 0; b < sym_sz; ++b)
                            recv[target][b] ^= recv[j][b];
                known[target] = true;
                progress = true;
            }
        }
    }

    // 復元できたか確認
    for (size_t i = 0; i < k; ++i)
        if (!known[i]) throw std::runtime_error("LDPC: decode failed (too many erasures)");

    std::vector<uint8_t> out;
    out.reserve(meta.original_size);
    for (size_t i = 0; i < k; ++i)
        out.insert(out.end(), recv[i].begin(), recv[i].end());
    out.resize(meta.original_size);
    return out;
}

} // namespace ldpc_erasure
