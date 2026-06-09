#ifndef GENRE_DSL_CPP_INCLUDED
#define GENRE_DSL_CPP_INCLUDED
// genre_dsl.cpp  CPB Level 3 — ジャンル別 DSL 実装
#include "genre_dsl.hpp"
#include "container.hpp"
#include "dsl_vm.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <functional>

// ============================================================
// DSL バイト列書き込みヘルパー
// ============================================================
static void gw8 (std::vector<uint8_t>& o,uint8_t  v){o.push_back(v);}
static void gw16(std::vector<uint8_t>& o,uint16_t v){uint8_t b[2];memcpy(b,&v,2);o.insert(o.end(),b,b+2);}
static void gw32(std::vector<uint8_t>& o,uint32_t v){uint8_t b[4];memcpy(b,&v,4);o.insert(o.end(),b,b+4);}
static void gw64(std::vector<uint8_t>& o,uint64_t v){uint8_t b[8];memcpy(b,&v,8);o.insert(o.end(),b,b+8);}


// ── L3辞書 (スレッドローカル) ─────────────────────────────────
// l3_set_dict_path() で設定 → 次のエンコードで使用される
static thread_local std::string g_l3_dict_path;

void l3_set_dict_path(const std::string& path){
    g_l3_dict_path = path;
}

// Level 2 DSL builder (container.cpp)
extern std::vector<uint8_t> build_dsl(const std::vector<Blob>&, size_t);

// ============================================================
// ジャンル自動検出
// ============================================================
Genre detect_genre(const std::vector<uint8_t>& data, const std::string& hint)
{
    if (!hint.empty()) {
        std::string ext = hint;
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        if(ext==".txt"||ext==".json"||ext==".xml"||ext==".html"||
           ext==".csv"||ext==".md"||ext==".log")       return Genre::DOCUMENT;
        if(ext==".wav"||ext==".flac"||ext==".aiff"||ext==".pcm") return Genre::AUDIO;
        if(ext==".mp4"||ext==".mkv"||ext==".avi"||ext==".mov")   return Genre::VIDEO;
        if(ext==".png"||ext==".bmp"||ext==".tiff"||ext==".tif")  return Genre::IMAGE;
        if(ext==".atlas"||ext==".pak"||ext==".sprite")           return Genre::ASSET;
        if(ext==".db"||ext==".sqlite"||ext==".parquet"||ext==".tsv") return Genre::DATABASE;
    }
    if(data.size()<8) return Genre::UNKNOWN;
    const uint8_t* d=data.data();
    if(d[0]==0x89&&d[1]=='P'&&d[2]=='N'&&d[3]=='G') return Genre::IMAGE;
    if(d[0]=='B'&&d[1]=='M')                         return Genre::IMAGE;
    if(d[0]=='R'&&d[1]=='I'&&d[2]=='F'&&d[3]=='F'){
        if(data.size()>=12&&d[8]=='W'&&d[9]=='A'&&d[10]=='V'&&d[11]=='E') return Genre::AUDIO;
        if(data.size()>=12&&d[8]=='A'&&d[9]=='V'&&d[10]=='I')             return Genre::VIDEO;
    }
    if(d[0]=='f'&&d[1]=='L'&&d[2]=='a'&&d[3]=='C') return Genre::AUDIO;
    if(d[0]==0x1A&&d[1]==0x45&&d[2]==0xDF&&d[3]==0xA3) return Genre::VIDEO;
    if(data.size()>=8&&d[4]=='f'&&d[5]=='t'&&d[6]=='y'&&d[7]=='p') return Genre::VIDEO;
    size_t text=0, n=std::min(data.size(),size_t(512));
    for(size_t i=0;i<n;++i){uint8_t c=data[i];if(c==9||c==10||c==13||(c>=32&&c<=126)||c>=128)++text;}
    if(text>n*90/100) return Genre::DOCUMENT;
    return Genre::UNKNOWN;
}

// ============================================================
// 共通: Base DSL フォールバック
// ============================================================
static std::vector<uint8_t> base_encode(Genre g, const std::vector<Blob>& blobs, GenreMetadata& meta)
{
    meta.genre=g;
    size_t total=0; for(auto& b:blobs) total+=b.data.size();
    std::vector<uint8_t> dsl;
    gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)g);
    auto inner=build_dsl(blobs,total);
    dsl.insert(dsl.end(),inner.begin(),inner.end());
    gw8(dsl,BaseOp::GENRE_END);
    return dsl;
}
// genre_dsl_vm.cpp で定義 (前方宣言)
std::vector<uint8_t> run_genre_vm(
    const std::vector<uint8_t>& code,
    const std::vector<Blob>&    blobs,
    const GenreMetadata&        meta);

static std::vector<uint8_t> base_decode(const std::vector<uint8_t>& dsl,
                                         const std::vector<Blob>& blobs,
                                         const GenreMetadata&)
{
    size_t start=0;
    if(dsl.size()>=2&&dsl[0]==BaseOp::GENRE_BEGIN) start=2;
    size_t end=dsl.size();
    for(size_t i=start;i<dsl.size();++i) if(dsl[i]==BaseOp::GENRE_END){end=i;break;}
    std::vector<uint8_t> inner(dsl.begin()+start,dsl.begin()+end);
    return run_vm(inner,blobs);
}

// ============================================================
// ─── アセット系 (優先度1) ────────────────────────────────────
// ============================================================
// 戦略:
//   1. 全Blobを「アセットプール」として登録
//   2. 完全一致するBlobペアを ASSET_BASE_REF + VARIANT_DELTA(0差分) で表現
//   3. XOR差分が小さいペアを VARIANT_DELTA で圧縮
//   4. それ以外は Base DSL にフォールバック
// ============================================================

// XOR 距離: 2つのバイト列の差分バイト数を返す
static size_t xor_distance(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    size_t diff=0;
    size_t n=std::min(a.size(),b.size());
    for(size_t i=0;i<n;++i) if(a[i]!=b[i]) ++diff;
    diff+=std::max(a.size(),b.size())-n;
    return diff;
}

struct AssetCodec : GenreCodec {
    AssetCodec(){ genre=Genre::ASSET; }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::ASSET;
        if(blobs.empty()) return base_encode(Genre::ASSET,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();

        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::ASSET);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        // 各Blobをどのように表現するか決定
        // base[i] = このBlobの元Blob番号 (-1=独立)
        std::vector<int> base_of(blobs.size(),-1);
        uint64_t dst_offset=0;

        for(size_t i=0;i<blobs.size();++i){
            const auto& bi=blobs[i].data;
            // 既出Blobとの最小XOR距離を探す
            int best_j=-1; size_t best_dist=bi.size()+1;
            for(size_t j=0;j<i;++j){
                const auto& bj=blobs[j].data;
                if(bj.size()!=bi.size()) continue; // サイズが違う場合はスキップ
                size_t d=xor_distance(bi,bj);
                if(d<best_dist){ best_dist=d; best_j=(int)j; }
            }

            // 差分率が20%未満ならVARIANT_DELTAで表現
            const float DELTA_THRESHOLD=0.20f;
            bool use_delta=(best_j>=0 && best_dist<(size_t)(bi.size()*DELTA_THRESHOLD));

            if(use_delta){
                // VARIANT_DELTA: [base_id:4][dst:8][delta_blob:4]
                // VM: out[dst+k] = blobs[base_id][k] XOR blobs[delta_bid][k]
                // delta_bid = i (現アセット自体はXOR差分ではなくそのまま)
                // 注意: VMの実装は base XOR delta = result なので
                //       delta = base XOR current を事前計算する必要がある
                // 現実装ではBlobを動的追加できないため、
                // 完全一致(xor_dist==0)はASET_BASE_REF として扱う
                if(best_dist==0){
                    // 完全一致 → 元アセットをそのまま参照
                    gw8(dsl,AssetOp::ASSET_BASE_REF);
                    gw32(dsl,(uint32_t)best_j);
                    gw64(dsl,dst_offset);
                    gw32(dsl,(uint32_t)bi.size());
                } else {
                    // 差分あり → VARIANT_DELTA (base XOR current)
                    // delta_bid = i: VMは blobs[best_j] XOR blobs[i] = current を計算
                    // これは base XOR (base XOR current) = current なので正しい
                    gw8(dsl,AssetOp::VARIANT_DELTA);
                    gw32(dsl,(uint32_t)best_j);
                    gw64(dsl,dst_offset);
                    gw32(dsl,(uint32_t)i);
                }
                base_of[i]=best_j;

                // メタ: バージョン系統記録
                MetaVersion mv;
                mv.version_id=(uint32_t)i;
                mv.parent_id=(uint32_t)best_j;
                mv.timestamp=0;
                snprintf(mv.label,sizeof(mv.label),"v%zu",i);
                meta_out.versions.push_back(mv);
            } else {
                // ASSET_BASE_REF: [asset_id:4][dst:8][len:4]
                // asset_id = blob index (プールとして扱う)
                gw8(dsl,AssetOp::ASSET_BASE_REF);
                gw32(dsl,(uint32_t)i);
                gw64(dsl,dst_offset);
                gw32(dsl,(uint32_t)bi.size());
            }
            dst_offset+=bi.size();
        }

        gw8(dsl,BaseOp::END);
        gw8(dsl,BaseOp::GENRE_END);
        return dsl;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    {
        return run_genre_vm(dsl,blobs,meta); // genre_vm が担当
    }
};

// ============================================================
// ─── 動画系 (優先度2) ────────────────────────────────────────
// ============================================================
// 戦略:
//   1. 各フレームBlobを前フレームと比較
//   2. 変化率が低い(=背景固定) → BACKGROUND_HOLD
//   3. ほぼ同一フレームが連続 → FRAME_RUN
//   4. 差分が小さい → MOTION_PATCH (XOR差分をBlobとして保持)
//   5. それ以外 → FRAME_BASE_REF (キーフレーム参照)
// ============================================================

struct VideoCodec : GenreCodec {
    VideoCodec(){ genre=Genre::VIDEO; }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::VIDEO;
        if(blobs.empty()) return base_encode(Genre::VIDEO,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();

        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::VIDEO);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        uint64_t dst_offset=0;
        size_t i=0;

        while(i<blobs.size()){
            const auto& bi=blobs[i].data;

            // ── FRAME_RUN 検出: ほぼ同一フレームの連続 ──
            if(i+1<blobs.size()){
                size_t run=1;
                while(i+run<blobs.size()){
                    const auto& bj=blobs[i+run].data;
                    if(bj.size()!=bi.size()) break;
                    size_t d=xor_distance(bi,bj);
                    if(d > 0) break; // 完全一致のみ (lossless保証)
                    ++run;
                }
                if(run>=3){
                    // FRAME_RUN: [frame_id:4][count:2]
                    gw8(dsl,VidOp::FRAME_RUN);
                    gw32(dsl,(uint32_t)i);
                    gw16(dsl,(uint16_t)run);
                    // フレームメタ登録
                    for(size_t k=0;k<run;++k){
                        MetaFrameBoundary fb;
                        fb.frame_id=(uint32_t)(i+k);
                        fb.byte_offset=dst_offset+k*bi.size();
                        fb.flags=(k==0)?0x01:0x00;
                        meta_out.frames.push_back(fb);
                        dst_offset+=bi.size();
                    }
                    i+=run; continue;
                }
            }

            // ── BACKGROUND_HOLD 検出: 前フレームとの変化率が低い ──
            if(i>0){
                const auto& prev=blobs[i-1].data;
                if(prev.size()==bi.size()){
                    size_t d=xor_distance(bi,prev);
                    float change=(float)d/(float)bi.size();
                    if(change==0.0f){
                        // BACKGROUND_HOLD: 完全一致フレームのみ (前フレームをそのままコピー)
                        gw8(dsl,VidOp::BACKGROUND_HOLD);
                        gw32(dsl,(uint32_t)(i-1));
                        gw64(dsl,dst_offset-prev.size());
                        gw32(dsl,1u);
                        MetaFrameBoundary fb;
                        fb.frame_id=(uint32_t)i; fb.byte_offset=dst_offset; fb.flags=0x00;
                        meta_out.frames.push_back(fb);
                        dst_offset+=bi.size(); ++i; continue;
                    }
                    if(change<0.30f){
                        // MOTION_PATCH: [dst:8][patch_blob:4][len:4]
                        // patch_blob = 現フレームBlobのindexをXOR差分として扱う
                        gw8(dsl,VidOp::MOTION_PATCH);
                        gw64(dsl,dst_offset);
                        gw32(dsl,(uint32_t)i);
                        gw32(dsl,(uint32_t)bi.size());
                        MetaFrameBoundary fb;
                        fb.frame_id=(uint32_t)i; fb.byte_offset=dst_offset; fb.flags=0x00;
                        meta_out.frames.push_back(fb);
                        dst_offset+=bi.size(); ++i; continue;
                    }
                }
            }

            // ── FRAME_BASE_REF: キーフレームとして参照 ──
            gw8(dsl,VidOp::FRAME_BASE_REF);
            gw32(dsl,(uint32_t)i);
            gw64(dsl,dst_offset);
            gw32(dsl,(uint32_t)bi.size());
            MetaFrameBoundary fb;
            fb.frame_id=(uint32_t)i; fb.byte_offset=dst_offset; fb.flags=0x01;
            meta_out.frames.push_back(fb);
            dst_offset+=bi.size(); ++i;
        }

        gw8(dsl,BaseOp::END);
        gw8(dsl,BaseOp::GENRE_END);
        return dsl;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    {
        // REF_PHRASE/INDENT_PATTERN 等のジャンル命令を含むため
        // run_genre_vm を使う (run_vm は BaseOp のみ対応)
        return run_genre_vm(dsl, blobs, meta);
    }
};

// ============================================================
// ─── 文書系 (優先度3) ────────────────────────────────────────
// ============================================================
// 戦略:
//   1. 定型句辞書を構築 (4バイト以上、3回以上出現するフレーズ)
//   2. REF_PHRASE で辞書参照に置き換え
//   3. PARAGRAPH_DELTA で段落差分を記録
//   4. INDENT_PATTERN でインデント連続を圧縮
// ============================================================

// 簡易定型句辞書: フレーズ→ID のマップ
struct PhraseDict {
    static constexpr size_t MIN_PHRASE=4;    // 最小フレーズ長(4Bに短縮)
    static constexpr size_t MAX_PHRASE_LEN=32; // 最大フレーズ長   // 最小フレーズ長
    static constexpr size_t MAX_PHRASES=512; // 辞書サイズ拡大 // 最大辞書エントリ数

    std::vector<std::vector<uint8_t>> phrases; // id -> bytes
    std::unordered_map<std::string,uint16_t> index; // bytes -> id

    // データから頻出フレーズを抽出して辞書を構築
    void build(const std::vector<uint8_t>& data)
    {
        if(data.size()<MIN_PHRASE*2) return;
        // 複数の長さでN-gramを収集 (4B, 8B, 16B, 32B)
        std::unordered_map<std::string,int> freq;
        for(size_t len : {(size_t)4,(size_t)8,(size_t)16,(size_t)32}){
            if(len > data.size()) continue;
            for(size_t i=0;i+len<=data.size();++i){
                std::string ng((const char*)data.data()+i,len);
                freq[ng]++;
            }
        }
        // スコア = 頻度 × 長さ (長いフレーズを優先)
        std::vector<std::pair<int,std::string>> sorted;
        for(auto& [k,v]:freq){
            int score=v*(int)k.size();
            if(score >= (int)(MIN_PHRASE*3)) sorted.push_back({score,k});
        }
        std::sort(sorted.rbegin(),sorted.rend());
        for(auto& [sc,ng]:sorted){
            if(phrases.size()>=MAX_PHRASES) break;
            // より長い既登録フレーズに含まれるものはスキップ
            bool skip=false;
            for(auto& ph:phrases){
                if(ph.size()>ng.size()){
                    std::string ps((const char*)ph.data(),ph.size());
                    if(ps.find(ng)!=std::string::npos){skip=true;break;}
                }
            }
            if(skip) continue;
            uint16_t id=(uint16_t)phrases.size();
            phrases.push_back(std::vector<uint8_t>(ng.begin(),ng.end()));
            index[ng]=id;
        }
    }

    // 最長一致検索: マッチしたフレーズのIDと長さを返す
    std::pair<int16_t,size_t> lookup_best(const uint8_t* p, size_t avail) const {
        if(avail<MIN_PHRASE) return {-1,0};
        int16_t best_id=-1; size_t best_len=0;
        for(size_t len=std::min((size_t)32,avail); len>=MIN_PHRASE; --len){
            std::string k((const char*)p,len);
            auto it=index.find(k);
            if(it!=index.end()){ best_id=(int16_t)it->second; best_len=len; break; }
        }
        return {best_id, best_len};
    }
    int16_t lookup(const uint8_t* p, size_t avail) const {
        return lookup_best(p,avail).first;
    }

    bool load_from_file(const std::string& path){
        FILE* fp=fopen(path.c_str(),"rb"); if(!fp) return false;
        fseek(fp,0,SEEK_END); long sz=ftell(fp); rewind(fp);
        std::vector<uint8_t> raw(sz);
        if(sz>0) fread(raw.data(),1,sz,fp); fclose(fp);
        if(sz<13) return false;
        static const uint8_t M[8]={'C','P','B','D','I','C','T','!'};
        if(memcmp(raw.data(),M,8)!=0) return false;
        const uint8_t* p=raw.data()+9;
        uint32_t n=0; for(int i=0;i<4;i++) n|=(uint32_t(p[i])<<(i*8)); p+=4;
        phrases.clear(); index.clear();
        const uint8_t* en=raw.data()+raw.size();
        for(uint32_t i=0;i<n&&phrases.size()<MAX_PHRASES;i++){
            if(p+6>en) break;
            uint16_t len=p[0]|(p[1]<<8); p+=2;
            if(p+len+4>en) break;
            std::string key((const char*)p,len); p+=len+4;
            uint16_t id=(uint16_t)phrases.size();
            phrases.push_back(std::vector<uint8_t>(key.begin(),key.end()));
            index[key]=id;
        }
        return !phrases.empty();
    }
};

struct DocumentCodec : GenreCodec {
    DocumentCodec(){ genre=Genre::DOCUMENT; }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::DOCUMENT;
        if(blobs.empty()) return base_encode(Genre::DOCUMENT,blobs,meta_out);

        // 全データを連結
        std::vector<uint8_t> flat;
        for(auto& b:blobs) flat.insert(flat.end(),b.data.begin(),b.data.end());

        // 辞書構築
        PhraseDict dict;
        bool dict_loaded=false;
        if(!g_l3_dict_path.empty()){
            dict_loaded=dict.load_from_file(g_l3_dict_path);
        }
        if(!dict_loaded){
            dict.build(flat);
        }

        // 辞書が小さすぎたら Base DSL にフォールバック
        if(dict.phrases.size()<4) return base_encode(Genre::DOCUMENT,blobs,meta_out);

        size_t total=flat.size();
        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::DOCUMENT);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        // 辞書をDSLに埋め込む (CHECKPOINT 命令にラベルとして格納)
        // 実運用では専用のDICT_DEFINEセクションが望ましいが
        // 今は辞書BlobをREF_BLOBで参照する方針
        // → シンプルに: 辞書フレーズはあらかじめBlobとして保持されている前提
        // ここではINDENT_PATTERNとPARAGRAPH_DELTAの実装に注力

        // INDENT_PATTERN 検出: 連続したタブ/スペースの繰り返し
        size_t pos=0;

        while(pos<total){
            uint8_t c=flat[pos];

            // ── INDENT_PATTERN: 行頭の空白/タブ連続 ──
            if(c==' '||c=='\t'){
                size_t run=0; uint8_t indent_ch=c;
                while(pos+run<total && flat[pos+run]==indent_ch) ++run;
                if(run>=PhraseDict::MIN_PHRASE){
                    // INDENT_PATTERN: [dst:8][len:4][depth:1][ch:1]
                    gw8(dsl,DocOp::INDENT_PATTERN);
                    gw64(dsl,(uint64_t)pos);
                    gw32(dsl,(uint32_t)run);
                    gw8(dsl,(uint8_t)(run));
                    gw8(dsl,indent_ch);
                    pos+=run; continue;
                }
            }

            // ── 数値列圧縮: 連続する数値文字 (タイムスタンプ・IDなど) ──
            if((isdigit(flat[pos])||flat[pos]=='-')&&pos+6<=total){
                size_t nend=pos;
                while(nend<total&&(isdigit(flat[nend])||flat[nend]=='.'||
                      flat[nend]=='-'||flat[nend]=='e'||flat[nend]=='E'||
                      flat[nend]=='+')) nend++;
                size_t nlen=nend-pos;
                if(nlen>=6){ // 6文字以上の数値列をINLINE_DATAで圧縮
                    gw8(dsl,BaseOp::INLINE_DATA);
                    gw64(dsl,(uint64_t)pos);
                    gw32(dsl,(uint32_t)nlen);
                    dsl.insert(dsl.end(),flat.begin()+pos,flat.begin()+nend);
                    pos=nend; continue;
                }
            }

            // ── REF_PHRASE: 辞書フレーズ参照 (最長一致) ──
            if(pos+PhraseDict::MIN_PHRASE<=total){
                auto [pid,plen]=dict.lookup_best(flat.data()+pos,total-pos);
                if(pid>=0 && plen>=PhraseDict::MIN_PHRASE){
                    // REF_PHRASE: [phrase_id:2][dst:8][len:4]  ← lenを追加
                    gw8(dsl,DocOp::REF_PHRASE);
                    gw16(dsl,(uint16_t)pid);
                    gw64(dsl,(uint64_t)pos);
                    gw32(dsl,(uint32_t)plen); // 実際のマッチ長を記録
                    pos+=plen; continue;
                }
            }

            // ── Base RAW: flat の絶対位置からblobを逆算して発行 ──
            {
                size_t raw_start = pos;
                size_t raw_len   = 0;
                // 特殊パターンが現れるまでバイトを蓄積
                while(pos < total){
                    if(pos+PhraseDict::MIN_PHRASE<=total &&
                       dict.lookup(flat.data()+pos,total-pos)>=0) break;
                    if(flat[pos]==' '||flat[pos]=='\t'){
                        size_t r=0;
                        while(pos+r<total&&flat[pos+r]==flat[pos])++r;
                        if(r>=PhraseDict::MIN_PHRASE) break;
                    }
                    // 数値文字列の開始もbreak
                    if((flat[pos]=='-'||isdigit(flat[pos]))&&pos+4<=total){
                        size_t r=0;
                        while(pos+r<total&&(isdigit(flat[pos+r])||flat[pos+r]=='.'||
                              flat[pos+r]=='-'||flat[pos+r]=='e'||flat[pos+r]=='E'))++r;
                        if(r>=6) break;
                    }
                    ++pos; ++raw_len;
                }
                if(raw_len > 0){
                    // flat[raw_start..raw_start+raw_len) を
                    // blob 配列の座標に変換して RAW 命令を発行
                    // (各BLOBが連続している前提で絶対offset → blob_id + local_offset)
                    size_t remain  = raw_start;
                    size_t b_idx   = 0;
                    while(b_idx < blobs.size() &&
                          remain >= blobs[b_idx].data.size()){
                        remain -= blobs[b_idx].data.size();
                        ++b_idx;
                    }
                    if(b_idx < blobs.size()){
                        size_t b_off  = remain;
                        size_t b_avail= blobs[b_idx].data.size() - b_off;
                        size_t emit   = std::min(raw_len, b_avail);
                        // 1blob内に収まる範囲だけ発行、残りは次ループへ
                        gw8(dsl, BaseOp::RAW);
                        gw32(dsl,(uint32_t)b_idx);
                        gw64(dsl,(uint64_t)b_off);
                        gw64(dsl,(uint64_t)raw_start);
                        gw32(dsl,(uint32_t)emit);
                        // emit < raw_len のとき残りはループで処理される
                        // pos を emit 分だけ戻す (蓄積し過ぎた分の修正)
                        if(emit < raw_len){
                            pos = raw_start + emit; // 未処理分に戻す
                        }
                    } else {
                        // blob外 (通常発生しないが安全のため)
                        ++pos; // 1バイトスキップして続ける
                    }
                }
            }
        }

        gw8(dsl,BaseOp::END);
        gw8(dsl,BaseOp::GENRE_END);

        // 辞書が効いているか確認: Base DSLより大きければフォールバック
        auto fallback=base_encode(Genre::DOCUMENT,blobs,meta_out);
        if(dsl.size()>=fallback.size()) return fallback;
        return dsl;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    {
        // REF_PHRASE/INDENT_PATTERN 等のジャンル命令を含むため
        // run_genre_vm を使う (run_vm は BaseOp のみ対応)
        return run_genre_vm(dsl, blobs, meta);
    }
};

// ============================================================
// ─── 画像系 (優先度5) ────────────────────────────────────────
// ============================================================
// 戦略:
//   1. 同一タイル検出 → TILE_REPEAT
//   2. 左右反転で一致 → MIRROR_REGION
//   3. 単色領域 → LINE_BLOCK (単色塗り)
//   4. 部分差分 → PATCH_REGION
//   5. それ以外 → Base DSL
// ============================================================
struct ImageCodec : GenreCodec {
    ImageCodec(){ genre=Genre::IMAGE; }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::IMAGE;
        if(blobs.empty()) return base_encode(Genre::IMAGE,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();
        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::IMAGE);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        uint64_t dst=0;
        for(size_t i=0;i<blobs.size();++i){
            const auto& bi=blobs[i].data;

            // ── 単色チェック: 全バイトが同じ → LINE_BLOCK ──
            if(!bi.empty()){
                uint8_t c=bi[0]; bool mono=true;
                for(auto x:bi) if(x!=c){mono=false;break;}
                if(mono){
                    // LINE_BLOCK: [dst:8][len:4][dir:1][color:4]
                    // dir=0xFF で「単色塗り」を意味する拡張解釈
                    gw8(dsl,ImgOp::LINE_BLOCK);
                    gw64(dsl,dst); gw32(dsl,(uint32_t)bi.size());
                    gw8(dsl,0xFF); // dir=単色塗り
                    // color: 4バイト (R=G=B=A=c)
                    gw8(dsl,c); gw8(dsl,c); gw8(dsl,c); gw8(dsl,c);
                    dst+=bi.size(); continue;
                }
            }

            // ── TILE_REPEAT 検出: bi の前半と後半が一致する ──
            // タイルサイズを bi.size()/2, /4 で試す
            bool tiled=false;
            for(size_t tile_sz: {bi.size()/2, bi.size()/4}){
                if(tile_sz<4 || bi.size()%tile_sz!=0) continue;
                bool match=true;
                for(size_t k=tile_sz;k<bi.size();k+=tile_sz)
                    for(size_t j=0;j<tile_sz;++j)
                        if(bi[j]!=bi[k+j]){match=false;break;}
                if(match){
                    // RAWで先頭タイルを出力してからTILE_REPEATで複製
                    gw8(dsl,BaseOp::RAW);
                    gw32(dsl,(uint32_t)i); gw64(dsl,0); gw64(dsl,dst); gw32(dsl,(uint32_t)tile_sz);
                    // TILE_REPEAT: [tile_src:8][tile_w:2][tile_h:2][dst_x:4][dst_y:4][count_x:2][count_y:2]
                    gw8(dsl,ImgOp::TILE_REPEAT);
                    gw64(dsl,dst);                               // tile_src
                    gw32(dsl,(uint32_t)tile_sz);                 // tile_w (uint32_t)
                    gw32(dsl,1u);                                // tile_h (uint32_t)
                    gw32(dsl,(uint32_t)(bi.size()/tile_sz));     // count_x
                    gw32(dsl,1u);                                // count_y
                    dst+=bi.size(); tiled=true; break;
                }
            }
            if(tiled) continue;

            // ── MIRROR_REGION 検出: 前Blobと左右反転で一致 ──
            if(i>0){
                const auto& prev=blobs[i-1].data;
                if(prev.size()==bi.size()){
                    auto rev=bi; std::reverse(rev.begin(),rev.end());
                    if(rev==prev){
                        // MIRROR_REGION: [src:8][dst:8][len:4][axis:1]
                        gw8(dsl,ImgOp::MIRROR_REGION);
                        gw64(dsl,dst-prev.size()); // src=前Blob位置
                        gw64(dsl,dst);
                        gw32(dsl,(uint32_t)bi.size());
                        gw8(dsl,0x00); // axis=水平反転
                        dst+=bi.size(); continue;
                    }
                }
            }

            // ── PATCH_REGION 検出: 前Blobとの差分が小さい ──
            if(i>0){
                const auto& prev=blobs[i-1].data;
                if(prev.size()==bi.size()){
                    size_t diff=xor_distance(bi,prev);
                    if(diff<bi.size()*0.15f){
                        // PATCH_REGION: [dst:8][patch_blob:4][len:4]
                        gw8(dsl,ImgOp::PATCH_REGION);
                        gw64(dsl,dst);
                        gw32(dsl,(uint32_t)i);
                        gw32(dsl,(uint32_t)bi.size());
                        dst+=bi.size(); continue;
                    }
                }
            }

            // ── Base RAW ──
            gw8(dsl,BaseOp::RAW);
            gw32(dsl,(uint32_t)i); gw64(dsl,0); gw64(dsl,dst); gw32(dsl,(uint32_t)bi.size());
            dst+=bi.size();
        }

        gw8(dsl,BaseOp::END); gw8(dsl,BaseOp::GENRE_END);
        auto fallback=base_encode(Genre::IMAGE,blobs,meta_out);
        return dsl.size()<fallback.size() ? dsl : fallback;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    { return run_genre_vm(dsl,blobs,meta); }
};

// ============================================================
// ─── 音声系 (優先度7) ────────────────────────────────────────
// ============================================================
// 戦略:
//   1. 無音検出 (値が閾値以下) → SILENCE_RUN
//   2. ループ検出 (周期パターン) → LOOP_REF
//   3. 音声断片の再利用 → PHRASE_SAMPLE_REF
//   4. それ以外 → Base DSL
// ============================================================
struct AudioCodec : GenreCodec {
    AudioCodec(){ genre=Genre::AUDIO; }

    static constexpr uint8_t SILENCE_THRESHOLD = 4; // |sample| <= 4 で無音

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::AUDIO;
        if(blobs.empty()) return base_encode(Genre::AUDIO,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();
        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::AUDIO);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        uint64_t dst=0;
        for(size_t i=0;i<blobs.size();++i){
            const auto& bi=blobs[i].data;
            size_t pos=0;

            while(pos<bi.size()){
                // ── SILENCE_RUN: 無音区間 ──
                // 8バイト境界で無音チェック (16bit stereo想定: 4バイト/フレーム)
                size_t sil=0;
                while(pos+sil<bi.size() && bi[pos+sil]<=SILENCE_THRESHOLD) ++sil;
                if(sil>=16){ // 最小16サンプル
                    // SILENCE_RUN: [dst:8][samples:4][channels:1]
                    gw8(dsl,AudOp::SILENCE_RUN);
                    gw64(dsl,dst+pos);
                    gw32(dsl,(uint32_t)sil);
                    gw8(dsl,2u); // stereo
                    pos+=sil; continue;
                }

                // ── LOOP_REF: ループ検出 (周期パターン) ──
                // 残りデータの前半と後半が一致するか確認
                size_t remain=bi.size()-pos;
                bool looped=false;
                for(size_t period: {remain/2, remain/4, remain/8}){
                    if(period<8 || remain%period!=0) continue;
                    bool match=true;
                    for(size_t k=period;k<remain&&match;k+=period)
                        for(size_t j=0;j<period&&match;++j)
                            if(bi[pos+j]!=bi[pos+k+j]) match=false;
                    if(match){
                        // 最初の周期をRAWで出力してからLOOP_REFで複製
                        gw8(dsl,BaseOp::RAW);
                        gw32(dsl,(uint32_t)i); gw64(dsl,(uint64_t)pos); gw64(dsl,dst+pos); gw32(dsl,(uint32_t)period);
                        // LOOP_REF: [src:8][dst:8][len:4][times:2]
                        gw8(dsl,AudOp::LOOP_REF);
                        gw64(dsl,dst+pos);                    // src=先頭周期の出力位置
                        gw64(dsl,dst+pos+period);             // dst=2周期目以降の開始
                        gw32(dsl,(uint32_t)period);           // len=周期長
                        gw16(dsl,(uint16_t)(remain/period-1)); // times (2回目以降の数)
                        pos+=remain; looped=true; break;
                    }
                }
                if(looped) break;

                // ── PHRASE_SAMPLE_REF: 前Blobと同じ断片 ──
                if(i>0){
                    const auto& prev=blobs[i-1].data;
                    size_t chunk=std::min(bi.size()-pos, prev.size());
                    if(chunk>=8 && std::equal(bi.begin()+pos,bi.begin()+pos+chunk,prev.begin())){
                        // PHRASE_SAMPLE_REF: [sample_blob:4][dst:8][len:4]
                        gw8(dsl,AudOp::PHRASE_SAMPLE_REF);
                        gw32(dsl,(uint32_t)(i-1));
                        gw64(dsl,dst+pos);
                        gw32(dsl,(uint32_t)chunk);
                        pos+=chunk; continue;
                    }
                }

                // ── Base RAW (残り全部) ──
                gw8(dsl,BaseOp::RAW);
                gw32(dsl,(uint32_t)i); gw64(dsl,(uint64_t)pos); gw64(dsl,dst+pos); gw32(dsl,(uint32_t)(bi.size()-pos));
                pos=bi.size();
            }
            dst+=bi.size();
        }

        gw8(dsl,BaseOp::END); gw8(dsl,BaseOp::GENRE_END);
        auto fallback=base_encode(Genre::AUDIO,blobs,meta_out);
        return dsl.size()<fallback.size() ? dsl : fallback;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    { return run_genre_vm(dsl,blobs,meta); }
};

// ============================================================
// ─── データベース系 (優先度6) ─────────────────────────────────
// ============================================================
// 戦略:
//   1. NULL/ゼロ多発 → SPARSE_MAP
//   2. 行の繰り返し → BLOCK_RANGE_REPEAT
//   3. 前行との差分が小さい → ROW_DELTA
//   4. 周期的な数値変化 → PERIODIC_SERIES
//   5. それ以外 → Base DSL
// ============================================================
struct DatabaseCodec : GenreCodec {
    DatabaseCodec(){ genre=Genre::DATABASE; }

    // 行サイズを推定: 最初のNewline位置、またはデフォルト64バイト
    static size_t guess_row_size(const std::vector<uint8_t>& data){
        for(size_t i=1;i<std::min(data.size(),size_t(4096));++i)
            if(data[i]=='\n') return i+1;
        return 64;
    }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::DATABASE;
        if(blobs.empty()) return base_encode(Genre::DATABASE,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();
        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::DATABASE);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        uint64_t dst=0;
        for(size_t bi_idx=0;bi_idx<blobs.size();++bi_idx){
            const auto& bi=blobs[bi_idx].data;
            size_t row_sz=guess_row_size(bi);
            size_t pos=0;

            while(pos<bi.size()){
                size_t remain=bi.size()-pos;
                size_t chunk=std::min(remain,row_sz);

                // ── SPARSE_MAP: ゼロ/NULL 率が80%超 ──
                size_t zeros=0;
                for(size_t k=0;k<chunk;++k) if(bi[pos+k]==0||bi[pos+k]==',') ++zeros;
                if(zeros>=chunk*8/10){
                    // SPARSE_MAP: [dst:8][len:4][null_val:1]
                    gw8(dsl,DbOp::SPARSE_MAP);
                    gw64(dsl,dst+pos); gw32(dsl,(uint32_t)chunk); gw8(dsl,0x00);
                    pos+=chunk; continue;
                }

                // ── BLOCK_RANGE_REPEAT: 行が繰り返されている ──
                if(pos+chunk*2<=bi.size() &&
                   std::equal(bi.begin()+pos,bi.begin()+pos+chunk,bi.begin()+pos+chunk)){
                    size_t rep=1;
                    while(pos+(rep+1)*chunk<=bi.size() &&
                          std::equal(bi.begin()+pos,bi.begin()+pos+chunk,
                                     bi.begin()+pos+rep*chunk)) ++rep;
                    if(rep>=2){
                        // 最初の行をRAWで出力してからBLOCK_RANGE_REPEATで複製
                        gw8(dsl,BaseOp::RAW);
                        gw32(dsl,(uint32_t)bi_idx); gw64(dsl,(uint64_t)pos);
                        gw64(dsl,dst+pos); gw32(dsl,(uint32_t)chunk);
                        // BLOCK_RANGE_REPEAT: [src:8][dst:8][row_len:4][count:4]
                        gw8(dsl,DbOp::BLOCK_RANGE_REPEAT);
                        gw64(dsl,dst+pos);           // src=先頭行の出力位置
                        gw64(dsl,dst+pos+chunk);     // dst=2行目以降の開始
                        gw32(dsl,(uint32_t)chunk);   // row_len
                        gw32(dsl,(uint32_t)(rep-1)); // count (2行目以降の数)
                        pos+=chunk*rep; continue;
                    }
                }

                // ── ROW_DELTA: 前行との差分が小さい ──
                if(pos>=row_sz){
                    size_t diff=0;
                    for(size_t k=0;k<chunk;++k)
                        if(bi[pos+k]!=bi[pos-row_sz+k]) ++diff;
                    if(diff<chunk/4){ // 25%未満の変化
                        // ROW_DELTA: [prev_row:8][dst:8][delta:4]
                        gw8(dsl,DbOp::ROW_DELTA);
                        gw64(dsl,dst+pos-row_sz);    // prev_row
                        gw64(dsl,dst+pos);           // dst
                        gw32(dsl,(uint32_t)bi_idx);  // delta blob (current row)
                        pos+=chunk; continue;
                    }
                }

                // ── Base RAW ──
                gw8(dsl,BaseOp::RAW);
                gw32(dsl,(uint32_t)bi_idx); gw64(dsl,(uint64_t)pos);
                gw64(dsl,dst+pos); gw32(dsl,(uint32_t)chunk);
                pos+=chunk;
            }
            dst+=bi.size();
        }

        gw8(dsl,BaseOp::END); gw8(dsl,BaseOp::GENRE_END);
        auto fallback=base_encode(Genre::DATABASE,blobs,meta_out);
        return dsl.size()<fallback.size() ? dsl : fallback;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    { return run_genre_vm(dsl,blobs,meta); }
};

// ============================================================
// ─── 混合系 (優先度4) ────────────────────────────────────────
// ============================================================
// 戦略:
//   1. 複数Blob間の完全一致 → SHARED_BLOB_POOL
//   2. Blob間の差分が小さい → SNAPSHOT_DELTA
//   3. マニフェスト(最初のBlob) → MANIFEST_REF
//   4. それ以外 → Base DSL
// ============================================================
struct MixedCodec : GenreCodec {
    MixedCodec(){ genre=Genre::MIXED; }

    std::vector<uint8_t> encode(const std::vector<Blob>& blobs, GenreMetadata& meta_out) override
    {
        meta_out.genre=Genre::MIXED;
        if(blobs.empty()) return base_encode(Genre::MIXED,blobs,meta_out);

        size_t total=0; for(auto& b:blobs) total+=b.data.size();
        std::vector<uint8_t> dsl;
        gw8(dsl,BaseOp::GENRE_BEGIN); gw8(dsl,(uint8_t)Genre::MIXED);
        gw8(dsl,BaseOp::OUTPUT); gw64(dsl,(uint64_t)total);

        // 最初のBlobをマニフェストとして扱う
        if(!blobs.empty()){
            // MANIFEST_REF: [manifest_blob:4][dst:8]
            gw8(dsl,MixOp::MANIFEST_REF);
            gw32(dsl,0u); gw64(dsl,0ull);
        }

        uint64_t dst=0;
        // 重複Blobのインデックスを記録 (先に書いたBlobのmap)
        std::unordered_map<std::string,uint32_t> seen; // hash→blob_id

        for(size_t i=0;i<blobs.size();++i){
            const auto& bi=blobs[i].data;
            // 8バイトのフィンガープリントで同一Blobを検出
            std::string fp(bi.begin(), bi.begin()+std::min(bi.size(),size_t(32)));
            fp+=std::to_string(bi.size());

            auto it=seen.find(fp);
            if(it!=seen.end() && blobs[it->second].data==bi){
                // SHARED_BLOB_POOL: [pool_blob:4][dst:8][len:4]
                gw8(dsl,MixOp::SHARED_BLOB_POOL);
                gw32(dsl,it->second); gw64(dsl,dst); gw32(dsl,(uint32_t)bi.size());
                dst+=bi.size(); continue;
            }

            // SNAPSHOT_DELTA: 前Blobと差分が20%未満
            if(i>0){
                const auto& prev=blobs[i-1].data;
                if(prev.size()==bi.size()){
                    size_t diff=xor_distance(bi,prev);
                    if(diff<bi.size()*0.20f){
                        // SNAPSHOT_DELTA: [base_id:4][dst:8][delta:4]
                        gw8(dsl,MixOp::SNAPSHOT_DELTA);
                        gw32(dsl,(uint32_t)(i-1)); gw64(dsl,dst); gw32(dsl,(uint32_t)i);
                        seen[fp]=(uint32_t)i; dst+=bi.size(); continue;
                    }
                }
            }

            // Base RAW
            gw8(dsl,BaseOp::RAW);
            gw32(dsl,(uint32_t)i); gw64(dsl,0ull); gw64(dsl,dst); gw32(dsl,(uint32_t)bi.size());
            seen[fp]=(uint32_t)i; dst+=bi.size();
        }

        gw8(dsl,BaseOp::END); gw8(dsl,BaseOp::GENRE_END);
        auto fallback=base_encode(Genre::MIXED,blobs,meta_out);
        return dsl.size()<fallback.size() ? dsl : fallback;
    }

    std::vector<uint8_t> decode(const std::vector<uint8_t>& dsl,
                                 const std::vector<Blob>& blobs,
                                 const GenreMetadata& meta) override
    { return run_genre_vm(dsl,blobs,meta); }
};

// ============================================================
// Genre Registry
// ============================================================
std::unique_ptr<GenreCodec> make_genre_codec(Genre g)
{
    switch(g){
    case Genre::DOCUMENT: return std::make_unique<DocumentCodec>();
    case Genre::IMAGE:    return std::make_unique<ImageCodec>();
    case Genre::VIDEO:    return std::make_unique<VideoCodec>();
    case Genre::AUDIO:    return std::make_unique<AudioCodec>();
    case Genre::ASSET:    return std::make_unique<AssetCodec>();
    case Genre::DATABASE: return std::make_unique<DatabaseCodec>();
    case Genre::MIXED:    return std::make_unique<MixedCodec>();
    default:              return std::make_unique<DocumentCodec>();
    }
}

#endif // GENRE_DSL_CPP_INCLUDED

// ── L3辞書 直接フレーズ置換 ───────────────────────────────────────

static const uint8_t L3D_SIG[4]={0xC3,0xD1,0xC2,0x54};

std::vector<uint8_t> l3_dict_direct_encode(
    const std::vector<uint8_t>& data, const std::string& dict_path)
{
    if(dict_path.empty()) return data;
    PhraseDict dict;
    if(!dict.load_from_file(dict_path)||dict.phrases.size()<2) return data;
    std::vector<uint8_t> out;
    out.insert(out.end(),L3D_SIG,L3D_SIG+4);
    uint64_t osz=data.size();
    for(int i=0;i<8;i++) out.push_back((osz>>(i*8))&0xFF);
    size_t pos=0;
    while(pos<data.size()){
        auto [pid,plen]=dict.lookup_best(data.data()+pos,data.size()-pos);
        if(pid>=0&&plen>=(size_t)PhraseDict::MIN_PHRASE){
            out.push_back(0xFF); out.push_back(0x03);
            out.push_back(uint8_t(pid)); out.push_back(uint8_t(pid>>8));
            pos+=plen;
        } else {
            uint8_t c=data[pos++];
            if(c==0xFF) out.push_back(0xFF);
            out.push_back(c);
        }
    }
    return out.size()<data.size()?out:data;
}

bool is_l3_dict_format(const std::vector<uint8_t>& data){
    return data.size()>=12&&memcmp(data.data(),L3D_SIG,4)==0;
}

std::vector<uint8_t> l3_dict_direct_decode(
    const std::vector<uint8_t>& data, const std::string& dict_path)
{
    if(!is_l3_dict_format(data)) return data;
    PhraseDict dict;
    if(dict_path.empty()||!dict.load_from_file(dict_path)) return data;
    const uint8_t* p=data.data()+4;
    uint64_t osz=0; for(int i=0;i<8;i++) osz|=(uint64_t(p[i])<<(i*8)); p+=8;
    std::vector<uint8_t> out; out.reserve(osz);
    const uint8_t* en=data.data()+data.size();
    while(p<en){
        if(*p==0xFF&&p+1<en){
            if(*(p+1)==0x03&&p+4<=en){
                uint16_t id=p[2]|(p[3]<<8); p+=4;
                if(id<dict.phrases.size())
                    out.insert(out.end(),dict.phrases[id].begin(),dict.phrases[id].end());
            } else { out.push_back(0xFF); p+=2; }
        } else out.push_back(*p++);
    }
    return out;
}
