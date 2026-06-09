// rs_codec.cpp
// Reed-Solomon RS(n=block+parity, k=block) over GF(2^8)
// 原始多項式: x^8+x^4+x^3+x^2+1 (0x11D)
//
// コードワードレイアウト (低次 = index 0):
//   [parity[0..npar-1]] [data[0..block-1]]
//   c(x) = parity[0] + ... + parity[npar-1]*x^{npar-1}
//         + data[0]*x^npar + ... + data[block-1]*x^{n-1}
//
// シンドローム評価: S_i = c(α^i)  を peval (低次index0) で計算
// → 正しい符号語なら S_i = 0  (i=0..npar-1)
#include "rs_codec.hpp"
#include <stdexcept>
#include <cstring>
#include <mutex>

// ============================================================
// GF(2^8)
// ============================================================
namespace gf {
static constexpr uint16_t PRIM = 0x11D;
static uint8_t EXP[512], LOG[256];
static std::once_flag gf_init_flag;
static void init() {
    std::call_once(gf_init_flag, []{
    uint16_t x=1;
    for(int i=0;i<255;++i){EXP[i]=(uint8_t)x;LOG[x]=(uint8_t)i;x<<=1;if(x&0x100)x^=PRIM;}
    for(int i=255;i<512;++i) EXP[i]=EXP[i-255];
    LOG[0]=0;
    });
}
static inline uint8_t mul(uint8_t a,uint8_t b){if(!a||!b)return 0;return EXP[(int)LOG[a]+LOG[b]];}
static inline uint8_t inv(uint8_t a){if(!a)throw std::runtime_error("GF:inv(0)");return EXP[255-LOG[a]];}
static inline uint8_t div(uint8_t a,uint8_t b){return mul(a,inv(b));}
static inline uint8_t pw(int i){return EXP[((i%255)+255)%255];}
} // namespace gf

using Poly=std::vector<uint8_t>; // index0=最低次

static Poly pmul(const Poly&a,const Poly&b){
    Poly r(a.size()+b.size()-1,0);
    for(size_t i=0;i<a.size();++i)
        for(size_t j=0;j<b.size();++j) r[i+j]^=gf::mul(a[i],b[j]);
    return r;
}
static Poly pmod(Poly p,const Poly&d){
    int dd=(int)d.size();
    while((int)p.size()>=dd){
        uint8_t c=gf::div(p.back(),d.back());
        int off=(int)p.size()-dd;
        for(int i=0;i<dd;++i) p[off+i]^=gf::mul(d[i],c);
        p.pop_back();
    }
    return p;
}
// p(v) を低次 index0 で評価
static uint8_t peval(const Poly&p,uint8_t v){
    uint8_t y=0;
    for(int i=(int)p.size()-1;i>=0;--i) y=gf::mul(y,v)^p[i];
    return y;
}
static Poly make_gen(int npar){
    Poly g={1};
    for(int i=0;i<npar;++i) g=pmul(g,{gf::pw(i),1});
    return g;
}

// ============================================================
// 1ブロック符号化
// 出力: [parity(npar)][data(block)]  低次index0
// ============================================================
static std::vector<uint8_t> encode_block(
    const uint8_t* msg, size_t mlen, const Poly& gen, int npar)
{
    // shifted(x) = msg(x) * x^npar
    Poly shifted(mlen+npar,0);
    for(size_t i=0;i<mlen;++i) shifted[i+npar]=msg[i];
    Poly rem=pmod(shifted,gen);
    rem.resize(npar,0);

    // c(x) = rem(x) + shifted(x) = [parity][data]
    std::vector<uint8_t> cw(mlen+npar);
    for(int i=0;i<npar;++i)     cw[i]      =rem[i];   // パリティ先頭(低次)
    for(size_t i=0;i<mlen;++i)  cw[npar+i] =msg[i];   // データ後続(高次)
    return cw;
}

// ============================================================
// 1ブロック復号  BM + Chien + Forney
// cw: [parity(npar)][data(block)]  (encode_block と同じ)
// ============================================================
static std::vector<uint8_t> decode_block(
    const uint8_t* cw, size_t cwlen, int npar)
{
    // 1. シンドローム S_i = c(α^i)  低次index0評価
    Poly S(npar);
    bool bad=false;
    for(int i=0;i<npar;++i){
        S[i]=peval(Poly(cw,cw+cwlen), gf::pw(i));
        if(S[i]) bad=true;
    }
    if(!bad)
        return std::vector<uint8_t>(cw+npar, cw+cwlen);

    // 2. Berlekamp-Massey
    Poly L={1},B={1};
    int Ln=0,m=1; uint8_t b=1;
    for(int k=0;k<npar;++k){
        uint8_t d=S[k];
        for(int i=1;i<=Ln;++i) d^=gf::mul(L[i],S[k-i]);
        if(!d){++m;continue;}
        if(2*Ln<=k){
            Poly T=L;
            uint8_t c=gf::div(d,b);
            if(L.size()<B.size()+m) L.resize(B.size()+m,0);
            for(size_t i=0;i<B.size();++i) L[i+m]^=gf::mul(c,B[i]);
            Ln=k+1-Ln; B=T; b=d; m=1;
        } else {
            uint8_t c=gf::div(d,b);
            if(L.size()<B.size()+m) L.resize(B.size()+m,0);
            for(size_t i=0;i<B.size();++i) L[i+m]^=gf::mul(c,B[i]);
            ++m;
        }
    }
    if(Ln>npar/2) throw std::runtime_error("RS: too many errors");

    // 3. Chien Search
    // Λ の根は α^{-k}  → Λ(α^{-k})==0 のとき位置 k にエラー
    const int n=(int)cwlen;
    std::vector<int> pos;
    for(int k=0;k<n;++k)
        if(peval(L, gf::pw(-k))==0) pos.push_back(k);
    if((int)pos.size()!=Ln)
        throw std::runtime_error("RS: Chien search mismatch");

    // 4. Forney
    // Ω(x) = S(x)*Λ(x) mod x^npar
    Poly omega(npar,0);
    for(int i=0;i<npar;++i)
        for(int j=0;j<(int)L.size()&&j<=i;++j)
            omega[i]^=gf::mul(L[j],S[i-j]);
    // Λ'(x): 形式微分
    Poly Lp(L.size(),0);
    for(size_t i=1;i<L.size();i+=2) Lp[i-1]=L[i];

    std::vector<uint8_t> out(cw, cw+cwlen);
    for(int k : pos){
        // 根は α^{-k} なので X_k^{-1} = α^k
        // e_k = X_k^{-1} * Ω(X_k^{-1}) / Λ'(X_k^{-1})
        //      = α^k * Ω(α^{-k}) / Λ'(α^{-k})
        uint8_t Xk_inv = gf::pw(-k);  // α^{-k}
        uint8_t Xk     = gf::pw(k);   // α^k = X_k^{-1}
        uint8_t mag = gf::mul(Xk,
                      gf::div(peval(omega, Xk_inv), peval(Lp, Xk_inv)));
        out[k]^=mag;
    }
    return std::vector<uint8_t>(out.begin()+npar, out.end());
}

// ============================================================
// 公開 API
// ============================================================
std::vector<uint8_t> rs_encode_blockwise(
    const std::vector<uint8_t>& data, size_t block, size_t parity)
{
    gf::init();
    if(!block||!parity||block+parity>255)
        throw std::runtime_error("RS: invalid parameters");

    // ── サイズヘッダーをデータ先頭に組み込んでから符号化 ──
    // 旧: [8B:size(非保護)] + codewords
    // 新: codewords([size+data] を block 単位で RS 符号化)
    // → orig_size の 8 バイトも RS コードワード内に入り完全保護される

    const uint64_t orig = (uint64_t)data.size();
    // header(8B) + data を結合
    std::vector<uint8_t> payload;
    payload.reserve(8 + data.size());
    for(int i=0;i<8;++i) payload.push_back((uint8_t)((orig>>(i*8))&0xFF));
    payload.insert(payload.end(), data.begin(), data.end());

    Poly gen = make_gen((int)parity);
    const size_t cw = block + parity;
    size_t n_blocks = (payload.size() + block - 1) / block;
    // 出力バッファを事前確保 → push_back/insert ゼロ
    std::vector<uint8_t> out(n_blocks * cw, 0);

    for(size_t bi = 0; bi < n_blocks; ++bi) {
        size_t off = bi * block;
        size_t chunk = std::min(block, payload.size() - off);
        std::vector<uint8_t> pad(block, 0);
        std::memcpy(pad.data(), payload.data() + off, chunk);
        auto codeword = encode_block(pad.data(), block, gen, (int)parity);
        std::memcpy(out.data() + bi * cw, codeword.data(), cw);
    }
    return out;
}

std::vector<uint8_t> rs_decode_blockwise(
    const std::vector<uint8_t>& data, size_t block, size_t parity)
{
    gf::init();
    if(!block||!parity||block+parity>255)
        throw std::runtime_error("RS: invalid parameters");

    const size_t cw = block + parity;
    if(data.size() < cw)
        throw std::runtime_error("RS: data too short (less than one codeword)");
    if(data.size() % cw != 0)
        throw std::runtime_error("RS: payload not aligned");

    // 全コードワードを RS 復号 → payload([size(8B)] + [data] + padding)
    std::vector<uint8_t> payload;
    payload.reserve(data.size() / cw * block);
    const uint8_t* ptr = data.data();
    for(size_t off = 0; off < data.size(); off += cw) {
        auto blk = decode_block(ptr + off, cw, (int)parity);
        payload.insert(payload.end(), blk.begin(), blk.end());
    }

    // 先頭 8 バイトが orig_size (RS 保護下にある)
    if(payload.size() < 8)
        throw std::runtime_error("RS: recovered payload too short");
    uint64_t orig = 0;
    for(int i=0;i<8;++i) orig |= (uint64_t)payload[i] << (i*8);

    if(orig + 8 > payload.size())
        throw std::runtime_error("RS: recovered size mismatch");

    return std::vector<uint8_t>(payload.begin() + 8,
                                payload.begin() + 8 + (size_t)orig);
}
