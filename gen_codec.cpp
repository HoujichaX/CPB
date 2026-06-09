#ifndef GEN_CODEC_CPP_INCLUDED
#define GEN_CODEC_CPP_INCLUDED
// gen_codec.cpp — 辞書生成復元レイヤー実装
#include "cpb_helpers.hpp"
#include "gen_codec.hpp"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>

// ── シリアライズ ──
std::vector<uint8_t> GenInstruction::serialize() const {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)codec_id);
    for(int i=0;i<4;++i) b.push_back((uint8_t)((dict_id>>(i*8))&0xFF));
    for(int i=0;i<8;++i) b.push_back((uint8_t)((seed>>(i*8))&0xFF));
    for(int i=0;i<8;++i) b.push_back((uint8_t)((expected_size>>(i*8))&0xFF));
    uint16_t psz=(uint16_t)params.size();
    b.push_back(psz&0xFF); b.push_back(psz>>8);
    b.insert(b.end(),params.begin(),params.end());
    return b;
}
GenInstruction GenInstruction::deserialize(const std::vector<uint8_t>& buf) {
    if(buf.size()<23) throw std::runtime_error("GenInstruction: too short");
    GenInstruction g;
    g.codec_id=(GenreCodecId)buf[0];
    g.dict_id=0; for(int i=0;i<4;++i) g.dict_id|=(uint32_t)buf[1+i]<<(i*8);
    g.seed=0;    for(int i=0;i<8;++i) g.seed|=(uint64_t)buf[5+i]<<(i*8);
    g.expected_size=0; for(int i=0;i<8;++i) g.expected_size|=(uint64_t)buf[13+i]<<(i*8);
    uint16_t psz=buf[21]|(buf[22]<<8);
    if(buf.size()>=23u+psz)
        g.params.assign(buf.begin()+23,buf.begin()+23+psz);
    return g;
}

// ── GenDictionary ──
void GenDictionary::register_entry(GenreCodecId cid, GenDictEntry e){
    entries_[make_key(cid,e.id)]=std::move(e);
}
bool GenDictionary::has(GenreCodecId cid, uint32_t id) const {
    return entries_.count(make_key(cid,id))>0;
}
std::vector<uint8_t> GenDictionary::generate(const GenInstruction& ins) const {
    auto it=entries_.find(make_key(ins.codec_id,ins.dict_id));
    if(it==entries_.end())
        throw std::runtime_error("GenDictionary: unknown entry "+
                                 std::to_string(ins.dict_id));
    return it->second.generate(ins.seed, ins.params);
}
size_t GenDictionary::entry_count() const { return entries_.size(); }

GenDictionary::MatchResult GenDictionary::find_best(
    const std::vector<uint8_t>& data, double threshold) const
{
    MatchResult best;
    for(auto& [key,entry] : entries_){
        try {
            // seed=0, params=[] で生成してサイズとデータを確認
            auto gen=entry.generate(0, {});
            // サイズが違う場合は params でサイズを渡して再生成
            if(gen.size()!=data.size()){
                // data.size() を params に詰めて試みる
                std::vector<uint8_t> sz_params(4);
                uint32_t sz=(uint32_t)data.size();
                std::memcpy(sz_params.data(),&sz,4);
                gen=entry.generate(0, sz_params);
            }
            if(gen.size()!=data.size()) continue;
            size_t match=0;
            for(size_t i=0;i<data.size();++i) if(gen[i]==data[i]) ++match;
            double rate=(double)match/data.size();
            if(rate>=threshold && rate>best.match_rate){
                best.found=true;
                best.match_rate=rate;
                best.instr.codec_id=(GenreCodecId)(key>>24);
                best.instr.dict_id=key&0xFFFFFF;
                best.instr.seed=0;
                best.instr.expected_size=data.size();
                // 差分をparams に保存
                std::vector<uint8_t> patch;
                for(size_t i=0;i<data.size();++i)
                    if(gen[i]!=data[i]){ patch.push_back((uint8_t)i); patch.push_back(data[i]); }
                best.instr.params=patch;
                best.saved_bytes=data.size()-23-(uint32_t)patch.size();
            }
        } catch(...) {}
    }
    return best;
}

// ============================================================
// 組み込み辞書
// ============================================================

// ── 軽量PRNG ──
static uint8_t gen_lcg(uint64_t& s){s=s*6364136223846793005ULL+1442695040888963407ULL;return(uint8_t)(s>>56);}

// ── パッチ適用ヘルパー ──
// params = [offset1(1B), val1(1B), offset2, val2, ...] 相対オフセット
static void apply_patch(std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& patch)
{
    for(size_t i=0;i+1<patch.size();i+=2){
        size_t off=patch[i];
        if(off<data.size()) data[off]=patch[i+1];
    }
}

// ── 構造テンプレート辞書 ──
void register_struct_dict(GenDictionary& dict)
{
    // 0x01: JSON配列テンプレート {"items":[{...},...]}
    {
        GenDictEntry e;
        e.id=0x01; e.name="JSON_ARRAY"; e.out_size=0; // 可変
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t rows=10;
            if(params.size()>=4) memcpy(&rows,params.data(),4);
            std::string s="[\n";
            uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i){
                s+="  {\"id\":"+std::to_string(i);
                s+=",\"val\":"+std::to_string((int)(gen_lcg(st)%100));
                s+=",\"tag\":\"item_"+std::to_string(i)+"\"}";
                if(i<rows-1) s+=",";
                s+="\n";
            }
            s+="]\n";
            return {s.begin(),s.end()};
        };
        dict.register_entry(GenreCodecId::STRUCT, std::move(e));
    }
    // 0x02: CSV テンプレート
    {
        GenDictEntry e;
        e.id=0x02; e.name="CSV_TABLE"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t rows=20;
            if(params.size()>=4) memcpy(&rows,params.data(),4);
            std::string s="id,name,value,timestamp\n";
            uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i){
                s+=std::to_string(i)+",item_"+std::to_string(i);
                s+=","+std::to_string((int)(gen_lcg(st)%1000));
                s+=",2024-01-"+std::to_string(i%28+1)+"\n";
            }
            return {s.begin(),s.end()};
        };
        dict.register_entry(GenreCodecId::STRUCT, std::move(e));
    }
    // 0x03: HTML テンプレート
    {
        GenDictEntry e;
        e.id=0x03; e.name="HTML_PAGE"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            (void)seed;
            std::string title="Document";
            if(!params.empty()) title=std::string(params.begin(),params.end());
            std::string s="<!DOCTYPE html><html><head><title>"+title;
            s+="</title></head><body><h1>"+title+"</h1>";
            s+="<p>Generated content.</p></body></html>\n";
            return {s.begin(),s.end()};
        };
        dict.register_entry(GenreCodecId::STRUCT, std::move(e));
    }
}

// ── テクスチャ辞書 ──
void register_texture_dict(GenDictionary& dict)
{
    // 0x10: 単色テクスチャ
    {
        GenDictEntry e;
        e.id=0x10; e.name="MONO_COLOR"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096; uint8_t color=(uint8_t)(seed&0xFF);
            if(params.size()>=5){ memcpy(&sz,params.data(),4); color=params[4]; }
            std::vector<uint8_t> data(sz,color);
            return data;
        };
        dict.register_entry(GenreCodecId::TEXTURE, std::move(e));
    }
    // 0x11: タイルテクスチャ (周期パターン)
    {
        GenDictEntry e;
        e.id=0x11; e.name="TILE_PATTERN"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096, tile=64;
            if(params.size()>=8){ memcpy(&sz,params.data(),4); memcpy(&tile,params.data()+4,4); }
            if(tile==0) tile=1;
            uint64_t st=seed;
            std::vector<uint8_t> pat(tile);
            for(auto& x:pat) x=gen_lcg(st);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i) data[i]=pat[i%tile];
            return data;
        };
        dict.register_entry(GenreCodecId::TEXTURE, std::move(e));
    }
    // 0x12: グラデーション
    {
        GenDictEntry e;
        e.id=0x12; e.name="GRADIENT"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(params.size()>=4) memcpy(&sz,params.data(),4);
            uint8_t s0=(uint8_t)(seed&0xFF), s1=(uint8_t)((seed>>8)&0xFF);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i)
                data[i]=(uint8_t)(s0+(s1-s0)*(double)i/(sz>1?sz-1:1));
            return data;
        };
        dict.register_entry(GenreCodecId::TEXTURE, std::move(e));
    }
    // 0x13: ランダムノイズ (LCG)
    {
        GenDictEntry e;
        e.id=0x13; e.name="NOISE"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(params.size()>=4) memcpy(&sz,params.data(),4);
            uint64_t st=seed;
            std::vector<uint8_t> data(sz);
            for(auto& x:data) x=gen_lcg(st);
            return data;
        };
        dict.register_entry(GenreCodecId::TEXTURE, std::move(e));
    }
}

// ── 音声辞書 ──
void register_audio_dict(GenDictionary& dict)
{
    // 0x20: 無音
    {
        GenDictEntry e;
        e.id=0x20; e.name="SILENCE"; e.out_size=0;
        e.generate=[](uint64_t, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(params.size()>=4) memcpy(&sz,params.data(),4);
            return std::vector<uint8_t>(sz,0);
        };
        dict.register_entry(GenreCodecId::AUDIO, std::move(e));
    }
    // 0x21: ホワイトノイズ
    {
        GenDictEntry e;
        e.id=0x21; e.name="WHITE_NOISE"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(params.size()>=4) memcpy(&sz,params.data(),4);
            uint64_t st=seed;
            std::vector<uint8_t> data(sz);
            for(auto& x:data) x=gen_lcg(st);
            return data;
        };
        dict.register_entry(GenreCodecId::AUDIO, std::move(e));
    }
    // 0x22: 正弦波 (freq=params[4..7] Hz, amp=params[8])
    {
        GenDictEntry e;
        e.id=0x22; e.name="SINE_WAVE"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            uint32_t sz=4096; float freq=440.0f; uint8_t amp=127;
            if(params.size()>=4) memcpy(&sz,params.data(),4);
            if(params.size()>=8) memcpy(&freq,params.data()+4,4);
            if(params.size()>=9) amp=params[8];
            float phase=(float)(seed&0xFFFF)/65536.0f*6.2831853f;
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i)
                data[i]=(uint8_t)(128+amp*std::sin(phase+2*3.14159265f*freq*i/48000.0f));
            return data;
        };
        dict.register_entry(GenreCodecId::AUDIO, std::move(e));
    }
}

// ── 差分パッチ辞書 ──
void register_delta_dict(GenDictionary& dict)
{
    // 0x30: ゼロベースパッチ (全ゼロ + パッチ)
    {
        GenDictEntry e;
        e.id=0x30; e.name="ZERO_PATCH"; e.out_size=0;
        e.generate=[](uint64_t, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            if(params.size()<4) return {};
            uint32_t sz; memcpy(&sz,params.data(),4);
            std::vector<uint8_t> data(sz,0);
            // params[4..] = [offset, value, ...] ペア
            for(size_t i=4;i+1<params.size();i+=2)
                if(params[i]<sz) data[params[i]]=params[i+1];
            return data;
        };
        dict.register_entry(GenreCodecId::DELTA, std::move(e));
    }
    // 0x31: 前バージョンベースパッチ (外部参照が必要なため seed=前バージョンhash)
    {
        GenDictEntry e;
        e.id=0x31; e.name="VERSION_PATCH"; e.out_size=0;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& params)->std::vector<uint8_t>{
            if(params.size()<4) return {};
            uint32_t sz; memcpy(&sz,params.data(),4);
            // seed から前バージョンを再生成 (LCG)
            uint64_t st=seed;
            std::vector<uint8_t> data(sz);
            for(auto& x:data) x=gen_lcg(st);
            // パッチ適用
            for(size_t i=4;i+1<params.size();i+=2)
                if(params[i]<sz) data[params[i]]=params[i+1];
            return data;
        };
        dict.register_entry(GenreCodecId::DELTA, std::move(e));
    }
}

#endif
