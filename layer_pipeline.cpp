#ifndef LAYER_PIPELINE_CPP_INCLUDED
#define LAYER_PIPELINE_CPP_INCLUDED
// layer_pipeline.cpp — CPB パイプライン実行エンジン
// Phase 1+2: L2/L1実接続 + L3/L4A/L5 本接続
#include "layer_pipeline.hpp"
#include "compress_iface.hpp"
#include "protection_layer.hpp"
#include "genre_dsl.hpp"
#include "fourd_map.hpp"
#include "gen_codec.hpp"
#include "cpb_dict_protocol.hpp"
#include "dict_evolution.hpp"
#include "frame_index.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <random>
#include <numeric>
#include <memory>

std::unique_ptr<GenreCodec> make_genre_codec(Genre g);
std::vector<uint8_t> run_genre_vm(const std::vector<uint8_t>&,const std::vector<Blob>&,const GenreMetadata&);


static const uint8_t L3_MAGIC[4] = {0xC3,0xD5,0x1E,0x03};
static const uint8_t L5_MAGIC[4] = {0xC5,0x61,0x1C,0x05};

// DSL内のRAW命令をINLINE_DATAに変換する
// RAW [blob_id:4][src:8][dst:8][len:4] → INLINE_DATA [dst:8][len:4][data:len]
static int l3_op_size(uint8_t op); // 前方宣言

static std::vector<uint8_t> inline_dsl(
    const std::vector<uint8_t>& dsl,
    const std::vector<Blob>& blobs)
{
    std::vector<uint8_t> out;
    size_t pc = 0;
    while (pc < dsl.size()) {
        uint8_t op = dsl[pc++];
        // サイズが足りない場合は残りをそのままコピー
        auto copy_rest = [&](){
            out.push_back(op);
        };

        if (op == 0x02) { // BaseOp::RAW
            if (pc + 24 > dsl.size()) { copy_rest(); break; }
            uint32_t blob_id = 0;
            for(int i=0;i<4;++i) blob_id|=(uint32_t)(dsl[pc+i])<<(i*8); pc+=4;
            uint64_t src = 0;
            for(int i=0;i<8;++i) src|=(uint64_t)(dsl[pc+i])<<(i*8); pc+=8;
            uint64_t dst = 0;
            for(int i=0;i<8;++i) dst|=(uint64_t)(dsl[pc+i])<<(i*8); pc+=8;
            uint32_t len = 0;
            for(int i=0;i<4;++i) len|=(uint32_t)(dsl[pc+i])<<(i*8); pc+=4;

            if (blob_id < blobs.size() &&
                src + len <= blobs[blob_id].data.size()) {
                // INLINE_DATA [dst:8][len:4][data:len]
                out.push_back(0x09); // BaseOp::INLINE_DATA
                for(int i=0;i<8;++i) out.push_back((uint8_t)((dst>>(i*8))&0xFF));
                for(int i=0;i<4;++i) out.push_back((uint8_t)((len>>(i*8))&0xFF));
                const auto& bd = blobs[blob_id].data;
                out.insert(out.end(), bd.begin()+src, bd.begin()+src+len);
            }
            // 変換失敗は省略 (dest は 0 埋め)
        }
        else if (op == 0x01) { // OUTPUT [size:8]
            out.push_back(op);
            for(int i=0;i<8&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
        }
        else if (op == 0x03) { // COPY [src:8][dst:8][len:4]
            out.push_back(op);
            for(int i=0;i<20&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
        }
        else if (op == 0x04) { // FILL [dst:8][len:4][byte:1]
            out.push_back(op);
            for(int i=0;i<13&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
        }
        else if (op == 0x10) { // GENRE_BEGIN [genre:1]
            out.push_back(op);
            if(pc<dsl.size()) out.push_back(dsl[pc++]);
        }
        else if (op == 0xFF || op == 0x11) { // END / GENRE_END
            out.push_back(op); break;
        }
        else if (op == 0x09) { // INLINE_DATA [dst:8][len:4][data:len]
            out.push_back(op);
            if(pc + 12 > dsl.size()){ while(pc<dsl.size()) out.push_back(dsl[pc++]); break; }
            // dst(8)
            for(int i=0;i<8;++i) out.push_back(dsl[pc++]);
            // len(4)
            uint32_t ilen=0;
            for(int i=0;i<4;++i){ ilen|=(uint32_t)dsl[pc]<<(i*8); out.push_back(dsl[pc++]); }
            // data(ilen)
            for(uint32_t i=0;i<ilen&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
        }
        else if (op == 0x22) { // SECTION_TEMPLATE [tmpl_id:2][dst:8][param_len:2][params...]
            out.push_back(op);
            if(pc + 12 > dsl.size()){ while(pc<dsl.size()) out.push_back(dsl[pc++]); break; }
            for(int i=0;i<10;++i) out.push_back(dsl[pc++]); // tmpl_id(2)+dst(8)
            uint16_t plen=0;
            for(int i=0;i<2;++i){ plen|=(uint16_t)dsl[pc]<<(i*8); out.push_back(dsl[pc++]); }
            for(uint16_t i=0;i<plen&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
        }
        else {
            // l3_op_size テーブルで固定サイズ命令を汎用スキップ
            int sz = l3_op_size(op);
            if(sz >= 0){
                out.push_back(op);
                for(int i=0;i<sz&&pc<dsl.size();++i) out.push_back(dsl[pc++]);
            } else {
                // 不明な可変長命令: 残りをコピーして終了
                out.push_back(op);
                while(pc < dsl.size()) out.push_back(dsl[pc++]);
                break;
            }
        }
    }
    // 不完全変換チェック: 変換後にRAW命令(0x02)が残っていたら失敗
    for(size_t k=0; k<out.size(); ++k)
        if(out[k]==0x02) return {}; // 失敗 → 空を返す
    return out;
}

static std::vector<uint8_t> l3_encode(const std::vector<uint8_t>& data){
    Genre g=detect_genre(data); auto blobs=split_into_blobs(data);
    auto codec=make_genre_codec(g); GenreMetadata meta; meta.genre=g;
    std::vector<uint8_t> dsl;
    try{dsl=codec->encode(blobs,meta);}catch(...){return data;}

    // DSL内のRAW命令をINLINE_DATAに変換 → blobs不要に
    auto inlined = inline_dsl(dsl, blobs);

    // inline_dsl が失敗 (空を返した) または元データより大きければパススルー
    if(inlined.empty() || inlined.size() + 13 >= data.size()) return data;

    std::vector<uint8_t> out;
    for(auto c:L3_MAGIC) out.push_back(c);
    out.push_back((uint8_t)g);
    uint64_t orig=data.size();
    for(int i=0;i<8;++i) out.push_back((uint8_t)((orig>>(i*8))&0xFF));
    out.insert(out.end(), inlined.begin(), inlined.end());
    return out;
}
static std::vector<uint8_t> l3_decode(const std::vector<uint8_t>& data){
    if(data.size()<13||memcmp(data.data(),L3_MAGIC,4)!=0) return data;
    const uint8_t* p=data.data()+4;
    Genre g=(Genre)(*p++);
    uint64_t orig_sz=0;
    for(int i=0;i<8;++i) orig_sz|=(uint64_t)(*p++)<<(i*8);
    // 残りはINLINE済みDSL — blobsは不要
    size_t dsl_sz=data.size()-13;
    std::vector<uint8_t> dsl(p,p+dsl_sz);
    std::vector<Blob> empty_blobs;
    GenreMetadata meta; meta.genre=g;
    try{
        auto out=run_genre_vm(dsl,empty_blobs,meta);
        out.resize(orig_sz,0);
        return out;
    }catch(std::exception& e){
        // L3 decode exception - fallback to raw
        (void)e;
        return data;
    }catch(...){return data;}
}
static std::vector<uint8_t> l4a_shuffle(
    const std::vector<uint8_t>& data,uint64_t seed,bool fwd){
    if(data.empty())return data;
    size_t n=data.size(); std::vector<size_t> idx(n); std::iota(idx.begin(),idx.end(),0);
    std::mt19937_64 rng(seed^0xCB4D4D4DCB4D4DULL);
    for(size_t i=n-1;i>0;--i){std::uniform_int_distribution<size_t>d(0,i);std::swap(idx[i],idx[d(rng)]);}
    std::vector<uint8_t> out(n);
    if(fwd){for(size_t i=0;i<n;++i)out[idx[i]]=data[i];}
    else   {for(size_t i=0;i<n;++i)out[i]=data[idx[i]];}
    return out;
}

struct BuiltinPat{uint8_t id;
    std::function<std::vector<uint8_t>(size_t,uint64_t)>gen;
    std::function<bool(const std::vector<uint8_t>&,uint64_t&)>match;};
static std::vector<BuiltinPat> bps(){return{
    {0,[](size_t n,uint64_t){return std::vector<uint8_t>(n,0);},[](const std::vector<uint8_t>&d,uint64_t&){for(auto b:d)if(b)return false;return true;}},
    {1,[](size_t n,uint64_t s){return std::vector<uint8_t>(n,(uint8_t)s);},[](const std::vector<uint8_t>&d,uint64_t&s){if(d.empty())return false;uint8_t v=d[0];for(auto b:d)if(b!=v)return false;s=v;return true;}},
    {2,[](size_t n,uint64_t s){std::vector<uint8_t>r(n);for(size_t i=0;i<n;++i)r[i]=(uint8_t)((i+s)&0xFF);return r;},[](const std::vector<uint8_t>&d,uint64_t&s){if(d.empty())return false;uint8_t base=d[0];for(size_t i=0;i<d.size();++i)if(d[i]!=(uint8_t)((i+base)&0xFF))return false;s=base;return true;}},
    {3,[](size_t n,uint64_t s){uint8_t a=(uint8_t)(s>>8),b=(uint8_t)s;std::vector<uint8_t>r(n);for(size_t i=0;i<n;++i)r[i]=(i%2?b:a);return r;},[](const std::vector<uint8_t>&d,uint64_t&s){if(d.size()<4)return false;uint8_t a=d[0],b=d[1];if(a==b)return false;for(size_t i=0;i<d.size();++i)if(d[i]!=(i%2?b:a))return false;s=((uint64_t)a<<8)|b;return true;}},
};}

static std::vector<uint8_t> l5_encode(const std::vector<uint8_t>& data){
    for(auto& bp:bps()){uint64_t s=0;if(bp.match(data,s)){
        std::vector<uint8_t> out; for(auto c:L5_MAGIC)out.push_back(c);
        out.push_back(bp.id); uint64_t sz=data.size();
        for(int i=0;i<8;++i)out.push_back((uint8_t)((sz>>(i*8))&0xFF));
        for(int i=0;i<8;++i)out.push_back((uint8_t)((s>>(i*8))&0xFF));
        return out;}}
    return data;
}
static std::vector<uint8_t> l5_decode(const std::vector<uint8_t>& data){
    if(data.size()<4||memcmp(data.data(),L5_MAGIC,4)!=0)return data;
    const uint8_t* p=data.data()+4; uint8_t id=*p++;
    uint64_t sz=0,s=0;
    for(int i=0;i<8;++i)sz|=(uint64_t)(*p++)<<(i*8);
    for(int i=0;i<8;++i)s|=(uint64_t)(*p++)<<(i*8);
    for(auto& bp:bps())if(bp.id==id)return bp.gen((size_t)sz,s);
    return data;
}



// ============================================================
// L4B — 多次元シャッフル (A+C 設計)
// 各アクティブ次元ごとに独立したシャッフルパスを適用
// ============================================================

static const uint8_t L4B_MAGIC[4] = {0xC4,0xB4,0x4D,0x04};

// 次元IDリスト (適用順序固定)
enum class Dim4ID : uint8_t {
    Z=0,Y,X,W,C,B,BX,BY,FV,TH,TX,K,FA,T,E,BA
};

// サブシード派生 (次元IDごとに独立した鍵を生成)
static uint64_t derive_seed(uint64_t seed, Dim4ID dim){
    uint64_t s = seed ^ ((uint64_t)dim * 0x9e3779b97f4a7c15ULL);
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return s;
}

// Fisher-Yates シャッフル (フォワード)
static std::vector<uint8_t> shuffle_fwd(
    const std::vector<uint8_t>& data, uint64_t seed)
{
    if(data.size() < 2) return data;
    size_t n = data.size();
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    // xoshiro256** でシャッフル
    // SplitMix64 でシードを展開
    auto splitmix=[](uint64_t& x)->uint64_t{
        x+=0x9e3779b97f4a7c15ULL;
        uint64_t z=x; z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
        z=(z^(z>>27))*0x94d049bb133111ebULL; return z^(z>>31);
    };
    uint64_t xs[4]; auto sx=seed;
    xs[0]=splitmix(sx);xs[1]=splitmix(sx);
    xs[2]=splitmix(sx);xs[3]=splitmix(sx);
    auto xnext=[&]()->uint64_t{
        auto rotl=[](uint64_t x,int k){return(x<<k)|(x>>(64-k));};
        uint64_t r=rotl(xs[1]*5,7)*9; uint64_t t=xs[1]<<17;
        xs[2]^=xs[0];xs[3]^=xs[1];xs[1]^=xs[2];xs[0]^=xs[3];
        xs[2]^=t;xs[3]=rotl(xs[3],45); return r;
    };
    for(size_t i=n-1;i>0;--i){
        size_t j=xnext()%(i+1);
        std::swap(idx[i],idx[j]);
    }
    std::vector<uint8_t> out(n);
    for(size_t i=0;i<n;++i) out[idx[i]]=data[i];
    return out;
}

// Fisher-Yates 逆シャッフル
static std::vector<uint8_t> shuffle_inv(
    const std::vector<uint8_t>& data, uint64_t seed)
{
    if(data.size() < 2) return data;
    size_t n = data.size();
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    auto splitmix=[](uint64_t& x)->uint64_t{
        x+=0x9e3779b97f4a7c15ULL;
        uint64_t z=x; z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
        z=(z^(z>>27))*0x94d049bb133111ebULL; return z^(z>>31);
    };
    uint64_t xs[4]; auto sx=seed;
    xs[0]=splitmix(sx);xs[1]=splitmix(sx);
    xs[2]=splitmix(sx);xs[3]=splitmix(sx);
    auto xnext=[&]()->uint64_t{
        auto rotl=[](uint64_t x,int k){return(x<<k)|(x>>(64-k));};
        uint64_t r=rotl(xs[1]*5,7)*9; uint64_t t=xs[1]<<17;
        xs[2]^=xs[0];xs[3]^=xs[1];xs[1]^=xs[2];xs[0]^=xs[3];
        xs[2]^=t;xs[3]=rotl(xs[3],45); return r;
    };
    for(size_t i=n-1;i>0;--i){
        size_t j=xnext()%(i+1);
        std::swap(idx[i],idx[j]);
    }
    std::vector<uint8_t> out(n);
    for(size_t i=0;i<n;++i) out[i]=data[idx[i]];
    return out;
}

// アクティブ次元リストを取得 (適用順序固定)
static std::vector<Dim4ID> active_dims(const DimConfig& dc){
    std::vector<Dim4ID> v;
    if(dc.use_z)  v.push_back(Dim4ID::Z);
    if(dc.use_y)  v.push_back(Dim4ID::Y);
    if(dc.use_x)  v.push_back(Dim4ID::X);
    if(dc.use_w)  v.push_back(Dim4ID::W);
    if(dc.use_c)  v.push_back(Dim4ID::C);
    if(dc.use_b)  v.push_back(Dim4ID::B);
    if(dc.use_bx) v.push_back(Dim4ID::BX);
    if(dc.use_by) v.push_back(Dim4ID::BY);
    if(dc.use_fv) v.push_back(Dim4ID::FV);
    if(dc.use_th) v.push_back(Dim4ID::TH);
    if(dc.use_tx) v.push_back(Dim4ID::TX);
    if(dc.use_k)  v.push_back(Dim4ID::K);
    if(dc.use_fa) v.push_back(Dim4ID::FA);
    if(dc.use_t)  v.push_back(Dim4ID::T);
    if(dc.use_e)  v.push_back(Dim4ID::E);
    if(dc.use_ba) v.push_back(Dim4ID::BA);
    return v;
}

// L4B エンコード
static std::vector<uint8_t> l4b_encode(
    const std::vector<uint8_t>& data, const CPBConfig& cfg)
{
    if(data.empty()) return data;
    auto dims = active_dims(cfg.dims);
    if(dims.empty()) return data;  // 次元なし → パスルー

    // 各次元のシャッフルを順番に適用
    std::vector<uint8_t> cur = data;
    for(auto dim : dims){
        uint64_t sub = derive_seed(cfg.fourd_seed, dim);
        cur = shuffle_fwd(cur, sub);
    }

    // ヘッダ付きで出力
    // [MAGIC 4][ver 1][dim_flags 2][seed 8][data...]
    std::vector<uint8_t> out;
    out.insert(out.end(), L4B_MAGIC, L4B_MAGIC+4);
    out.push_back(2);  // version 2 (多次元対応)
    uint16_t flags = cfg.dims.to_flags();
    out.push_back(flags & 0xFF);
    out.push_back((flags >> 8) & 0xFF);
    for(int i=0;i<8;++i) out.push_back((cfg.fourd_seed>>(i*8))&0xFF);
    out.insert(out.end(), cur.begin(), cur.end());
    return out;
}

// L4B デコード
static std::vector<uint8_t> l4b_decode(
    const std::vector<uint8_t>& data, const CPBConfig& cfg)
{
    if(data.size() <= 15) return data;
    if(memcmp(data.data(), L4B_MAGIC, 4) != 0) return data;
    const uint8_t* p = data.data() + 4;
    uint8_t ver = *p++;

    DimConfig dc;
    uint64_t seed = cfg.fourd_seed;

    if(ver == 2){
        // v2: dim_flags + seed を読む
        uint16_t flags = p[0] | ((uint16_t)p[1] << 8); p += 2;
        dc = DimConfig::from_flags(flags);
        seed = 0;
        for(int i=0;i<8;++i) seed |= ((uint64_t)p[i]) << (i*8); p+=8;
    } else {
        // v1 (後方互換): 旧フォーマット
        // [dims 1][seed 8] → 4次元固定
        uint8_t old_dims = *p++;
        seed = 0;
        for(int i=0;i<8;++i) seed |= ((uint64_t)p[i]) << (i*8); p+=8;
        dc = DimConfig::dim4();
        (void)old_dims;
    }

    std::vector<uint8_t> cur(p, data.data() + data.size());
    auto dims = active_dims(dc);

    // 逆順で逆シャッフル
    for(int i=(int)dims.size()-1;i>=0;--i){
        uint64_t sub = derive_seed(seed, dims[i]);
        cur = shuffle_inv(cur, sub);
    }
    return cur;
}

// ============================================================
// L5_LEARN — L3 DSL出力を辞書に学習するレイヤー
// encode: L3のDSL出力を受け取り → DictIDに置換 or DictEvolutionに提出
// decode: DictID → DSLを復元 → L3 VMで実行
// ============================================================

static const uint8_t L5_LEARN_MAGIC[4] = {0xC5,0xCA,0xC4,0x08};

// セッション内ミニキャッシュ (同一実行内での重複登録を防ぐ)
// DslCacheEntry: layer_pipeline.hpp で定義
static std::vector<DslCacheEntry> s_dsl_cache;

static uint64_t hash64(const std::vector<uint8_t>& v) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (auto b : v) { h ^= b; h *= 0x100000001b3ULL; }
    return h;
}

// encode: data = L3が出力したDSLプログラム
static std::vector<uint8_t> l5_learn_encode(
    const std::vector<uint8_t>& dsl_data,
    const CPBConfig& cfg)
{
    if (dsl_data.empty()) return dsl_data;

    uint64_t h = hash64(dsl_data);

    // セッションキャッシュから既知エントリを検索
    for (size_t i = 0; i < s_dsl_cache.size(); ++i) {
        if (s_dsl_cache[i].hash == h &&
            s_dsl_cache[i].dsl == dsl_data)
        {
            // キャッシュヒット: インデックスをDictID代わりに使う
            // [L5_LEARN_MAGIC 4B][cache_idx 4B][orig_dsl_size 8B]
            std::vector<uint8_t> out;
            for (auto c : L5_LEARN_MAGIC) out.push_back(c);
            uint32_t idx = (uint32_t)i;
            for (int j=0;j<4;++j) out.push_back((uint8_t)((idx>>(j*8))&0xFF));
            uint64_t sz = dsl_data.size();
            for (int j=0;j<8;++j) out.push_back((uint8_t)((sz>>(j*8))&0xFF));
            return out; // 16B 固定
        }
    }

    // キャッシュミス: サイズ制限チェック
    // 大きいデータはキャッシュしない（辞書肥大化を防ぐ）
    static constexpr size_t L5_MAX_CACHE_BYTES = 512 * 1024; // 512KB
    if (dsl_data.size() <= L5_MAX_CACHE_BYTES) {
        DslCacheEntry entry{h, dsl_data, dsl_data.size()};
        s_dsl_cache.push_back(entry);
    }
    // ※大きいデータはパススルー（L2圧縮に任せる）
    return dsl_data; // パススルー (初回はそのまま、L2が続けて圧縮)
}

// decode: encode の逆
static std::vector<uint8_t> l5_learn_decode(
    const std::vector<uint8_t>& data,
    const CPBConfig& cfg)
{
    if (data.size() < 16 ||
        memcmp(data.data(), L5_LEARN_MAGIC, 4) != 0)
        return data; // L5_LEARNフォーマットでない → パススルー

    const uint8_t* p = data.data() + 4;
    uint32_t idx = 0;
    for (int j=0;j<4;++j) idx |= (uint32_t)(*p++)<<(j*8);
    uint64_t orig_sz = 0;
    for (int j=0;j<8;++j) orig_sz |= (uint64_t)(*p++)<<(j*8);

    // セッションキャッシュからDSLを復元
    if (idx < s_dsl_cache.size()) {
        return s_dsl_cache[idx].dsl;
    }

    // キャッシュにない (別セッション等) → 空を返す (要: 永続辞書)
    // 現実装ではセッション内のみ有効
    return data;
}

// セッションキャッシュをリセット (テスト用)

// L5 キャッシュ永続化 API
std::vector<DslCacheEntry> l5_cache_export() {
    return s_dsl_cache;
}

void l5_cache_import(const std::vector<DslCacheEntry>& entries) {
    for(auto& e : entries) {
        // 重複チェック
        bool dup = false;
        for(auto& ex : s_dsl_cache)
            if(ex.hash == e.hash && ex.dsl == e.dsl) { dup=true; break; }
        if(!dup) s_dsl_cache.push_back(e);
    }
}
void l5_learn_reset_cache() { s_dsl_cache.clear(); }

// キャッシュ統計
size_t l5_learn_cache_size() { return s_dsl_cache.size(); }

static std::vector<uint8_t> apply_encode(
    LayerID l,const std::vector<uint8_t>& data,const CPBConfig& cfg){
    switch(l){
    case LayerID::L2: {
        CompressMethod cm = static_cast<CompressMethod>(cfg.compress_method);
        if(cm == CompressMethod::AUTO) cm = cpb_auto_detect(data);
        return cpb_compress(data, cm, cfg.compress_level);
    }
    case LayerID::L1:
        if(!cfg.rs.blob_parity) return data; // parity=0 → パスルー
        {ProtectionParams p;p.codec=ProtectionCodec::REED_SOLOMON;p.rs_block=cfg.rs.blob_block;p.rs_parity=cfg.rs.blob_parity;auto enc=protect_encode(data,p);return enc.symbols.empty()?data:enc.symbols[0];}
    case LayerID::L3: {
        // L3辞書あり → 直接フレーズ置換 (確実・シンプル)
        if(!cfg.l3_dict_path.empty()){
            auto r=l3_dict_direct_encode(data,cfg.l3_dict_path);
            if(r.size()<data.size()) return r; // 効果あり
        }
        // 辞書なし or 効果なし → 通常GenreDSL
        l3_set_dict_path(cfg.l3_dict_path);
        auto r=l3_encode(data); l3_set_dict_path(""); return r;
    }
    case LayerID::L4A: return l4a_shuffle(data,cfg.fourd_seed,true);
    case LayerID::L4B: return l4b_encode(data, cfg);
    case LayerID::L5:       return l5_encode(data);
    case LayerID::L5_LEARN: return l5_learn_encode(data,cfg);
    case LayerID::FIDX:{
        // FIDXはデータの末尾にインデックスを付加 (encode)
        // run_pipeline_encode 内で処理されるのでここはパスルー
        return data;
    }
    default: return data;
    }
}

static std::vector<uint8_t> apply_decode(
    LayerID l,const std::vector<uint8_t>& data,const CPBConfig& cfg){
    switch(l){
    case LayerID::L2:{
        CompressMethod cm=static_cast<CompressMethod>(cfg.compress_method);
        return cpb_decompress(data,cm);
    }
    case LayerID::L1:
        if(!cfg.rs.blob_parity) return data;
        {ProtectionParams p;p.codec=ProtectionCodec::REED_SOLOMON;p.rs_block=cfg.rs.blob_block;p.rs_parity=cfg.rs.blob_parity;ProtectionEncoded m;m.codec=ProtectionCodec::REED_SOLOMON;m.params=p;m.original_size=data.size();m.k=1;m.n=1;try{return protect_decode(m,{{0,data}});}catch(...){return data;}}
    case LayerID::L3:
        if(is_l3_dict_format(data)) return l3_dict_direct_decode(data,cfg.l3_dict_path);
        return l3_decode(data);
    case LayerID::L4A: return l4a_shuffle(data,cfg.fourd_seed,false);
    case LayerID::L4B: return l4b_decode(data,cfg);
    case LayerID::L5:       return l5_decode(data);
    case LayerID::L5_LEARN: return l5_learn_decode(data,cfg);
    case LayerID::FIDX:{
        // FIDXは末尾インデックスを除去するだけ
        if(data.size()>8){
            size_t mp=data.size()-4;
            if(data[mp]==0xF1&&data[mp+1]==0xD7&&data[mp+2]==0xF1&&data[mp+3]==0xD7){
                size_t sp=mp-4;
                uint32_t is=0; for(int i=0;i<4;i++) is|=((uint32_t)data[sp+i])<<(i*8);
                if(is+8<=data.size()) return std::vector<uint8_t>(data.begin(),data.begin()+(data.size()-8-is));
            }
        }
        return data;
    }
    default: return data;
    }
}


PipelineResult run_pipeline_encode(
    const std::vector<uint8_t>& input,
    const LayerPipeline& pipeline,
    const CPBConfig& config){
    PipelineResult res; res.ctx.config=config;

    // 学習モードの場合、STANDARDプロファイルでも自動的にL3→L5_LEARNに変換
    LayerPipeline active = pipeline;
    if (config.learning && pipeline.profile != PipelineProfile::CUSTOM) {
        // L5 → L5_LEARN に置換, L5→L3の順を L3→L5_LEARNに逆転
        auto& ord = active.encode_order;
        // L5 を L5_LEARN に置換
        for (auto& l : ord)
            if (l == LayerID::L5) { l = LayerID::L5_LEARN; }
        // L5_LEARN が L3 より前にある場合は入れ替え
        for (size_t i = 1; i < ord.size(); ++i) {
            if (ord[i-1] == LayerID::L5_LEARN && ord[i] == LayerID::L3) {
                std::swap(ord[i-1], ord[i]); // L3→L5_LEARN に並び替え
            }
        }
    }

    res.ctx.pipeline=active;
    auto data=input;
    for(auto l:active.encode_order){
        size_t b=data.size(); data=apply_encode(l,data,config);
        res.ctx.stage_log.push_back({l,b,data.size()});
    }
    res.data=std::move(data); res.success=true; return res;
}
PipelineResult run_pipeline_decode(
    const std::vector<uint8_t>& input,
    const LayerPipeline& pipeline,
    const CPBConfig& config){
    PipelineResult res; res.ctx.config=config;
    LayerPipeline active = pipeline;
    if (config.learning && pipeline.profile != PipelineProfile::CUSTOM) {
        for (auto& l : active.encode_order)
            if (l == LayerID::L5) l = LayerID::L5_LEARN;
        auto& ord = active.encode_order;
        for (size_t i = 1; i < ord.size(); ++i)
            if (ord[i-1] == LayerID::L5_LEARN && ord[i] == LayerID::L3)
                std::swap(ord[i-1], ord[i]);
    }
    res.ctx.pipeline=active;
    auto data=input;
    for(auto l:active.decode_order()){
        size_t b=data.size(); data=apply_decode(l,data,config);
        res.ctx.stage_log.push_back({l,b,data.size()});
    }
    res.data=std::move(data); res.success=true; return res;
}
void PipelineResult::print_summary() const {
    printf("Pipeline %s  %zu B\n",success?"OK":"FAIL",data.size());
    for(auto& s:ctx.stage_log)
        printf("  %-20s %6zu → %6zu\n",layer_name(s.layer),s.size_before,s.size_after);
}

// テスト用ラッパー
std::vector<uint8_t> apply_encode_test(
    const std::vector<uint8_t>& data, const CPBConfig& cfg)
{ return apply_encode(LayerID::L4B, data, cfg); }

std::vector<uint8_t> apply_decode_test(
    const std::vector<uint8_t>& data, const CPBConfig& cfg)
{ return apply_decode(LayerID::L4B, data, cfg); }

#endif // LAYER_PIPELINE_CPP_INCLUDED// 全命令の固定パラメータサイズテーブル (0=END/GENRE_END, -1=可変長)
static int l3_op_size(uint8_t op) {
    // 返り値: パラメータのバイト数 (opcodeの後のバイト数)
    // -1 = 可変長 (変換不可、そのままコピー)
    switch(op){
    case 0x01: return 8;   // OUTPUT [size:8]
    case 0x02: return 24;  // RAW [blob_id:4][src:8][dst:8][len:4]
    case 0x03: return 20;  // COPY [src:8][dst:8][len:4]
    case 0x04: return 13;  // FILL [dst:8][len:4][byte:1]
    case 0x05: return 24;  // PATCH [src:8][dst:8][len:4][xor:4]
    case 0x06: return 16;  // REF_BLOB [blob_id:4][dst:8][len:4]
    case 0x08: return 4;   // CHECKPOINT [label:4]
    case 0x09: return -1;  // INLINE_DATA (可変)
    case 0x10: return 1;   // GENRE_BEGIN [genre:1]
    case 0x11: return 0;   // GENRE_END
    case 0x20: return 14;  // REF_PHRASE [phrase_id:2][dst:8][len:4]
    case 0x21: return 14;  // REF_SCHEMA [schema_id:2][dst:8][len:4]
    case 0x22: return -1;  // SECTION_TEMPLATE (可変)
    case 0x23: return 20;  // PARAGRAPH_DELTA [prev:8][dst:8][dlen:4]
    case 0x24: return 14;  // INDENT_PATTERN [dst:8][len:4][depth:1][ch:1]
    case 0x30: return 24;  // TILE_REPEAT [tile_src:8][w:4][h:4][cx:4][cy:4]
    case 0x31: return 14;  // PALETTE_REGION [palette_id:2][dst:8][len:4]
    case 0x32: return 16;  // ALPHA_MASK_REF [mask_blob:4][dst:8][len:4]
    case 0x33: return 21;  // MIRROR_REGION [src:8][dst:8][len:4][axis:1]
    case 0x34: return 17;  // LINE_BLOCK [dst:8][len:4][dir:1][color:4]
    case 0x40: return 16;  // FRAME_BASE_REF [blob_id:4][dst:8][len:4]
    case 0x41: return 16;  // MOTION_PATCH [dst:8][patch_blob:4][len:4]
    case 0x42: return 6;   // FRAME_RUN [frame_id:4][count:2]
    case 0x43: return 8;   // BACKGROUND_HOLD [frame_id:4][count:4]
    case 0x50: return 12;  // SILENCE_RUN [dst:8][len:4]
    case 0x51: return 16;  // LOOP_REF [blob_id:4][dst:8][count:4]
    case 0x52: return 16;  // CHANNEL_DERIVE [src:4][dst:8][len:4]
    case 0x60: return 16;  // ASSET_BASE_REF [asset_id:4][dst:8][len:4]
    case 0x61: return 12;  // VARIANT_DELTA [base_id:4][dst:8]
    case 0x63: return 22;  // ATLAS_REGION_REF [atlas_id:4][x:2][y:2][w:2][h:2][dst:8]
    case 0x65: return 16;  // SHARED_TEXTURE [tex_id:4][dst:8][len:4]
    case 0x70: return 18;  // COLUMN_DICT [col_id:2][dict_id:4][dst:8][len:4]
    case 0x71: return 20;  // SPARSE_MAP [col_id:2][dst:8][skip:4][take:4]
    case 0x72: return 24;  // ROW_DELTA [ref_row:8][dst:8][len:4]
    case 0x73: return 12;  // SCHEMA_REF [schema_id:2][dst:8][len:4]
    case 0x74: return 14;  // PERIODIC_SERIES [dst:8][period:2][count:4]
    case 0xFF: return 0;   // END
    default:   return -1;  // 不明命令 = 可変扱い
    }
}

// DSL内のRAW命令をINLINE_DATAに変換する
// 全命令サイズを正確に把握してバイト列を安全に変換

