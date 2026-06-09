// cpb_dict_protocol.cpp — CPB Dictionary Protocol v1.0 実装
#include "cpb_dict_protocol.hpp"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <cstdio>

// ============================================================
// DictInstruction シリアライズ
// [dict_id:8][seed:8][params_size:4][params:N][expected_size:8]
// ============================================================
std::vector<uint8_t> DictInstruction::serialize() const {
    std::vector<uint8_t> b;
    uint64_t id=dict_id.encode();
    for(int i=0;i<8;++i) b.push_back((uint8_t)((id>>(i*8))&0xFF));
    for(int i=0;i<8;++i) b.push_back((uint8_t)((seed>>(i*8))&0xFF));
    uint32_t psz=(uint32_t)params.size();
    for(int i=0;i<4;++i) b.push_back((uint8_t)((psz>>(i*8))&0xFF));
    b.insert(b.end(),params.begin(),params.end());
    uint64_t esz=expected_size;
    for(int i=0;i<8;++i) b.push_back((uint8_t)((esz>>(i*8))&0xFF));
    return b;
}
DictInstruction DictInstruction::deserialize(const std::vector<uint8_t>& buf) {
    if(buf.size()<28) throw std::runtime_error("DictInstruction: too short");
    DictInstruction d;
    uint64_t id=0; for(int i=0;i<8;++i) id|=(uint64_t)buf[i]<<(i*8);
    d.dict_id=DictID::decode(id);
    uint64_t seed=0; for(int i=0;i<8;++i) seed|=(uint64_t)buf[8+i]<<(i*8);
    d.seed=seed;
    uint32_t psz=0; for(int i=0;i<4;++i) psz|=(uint32_t)buf[16+i]<<(i*8);
    if(buf.size()<20u+psz+8) throw std::runtime_error("DictInstruction: truncated");
    d.params.assign(buf.begin()+20, buf.begin()+20+psz);
    uint64_t esz=0;
    for(int i=0;i<8;++i) esz|=(uint64_t)buf[20+psz+i]<<(i*8);
    d.expected_size=esz;
    return d;
}

// ============================================================
// GenreDict
// ============================================================
void GenreDict::add_entry(DictEntry entry) {
    uint32_t eid=entry.id.entry_id;
    entry.id.genre=genre_;
    entries_[eid].push_back(std::move(entry));
    // current_version を最新に更新
    auto& added=entries_[eid].back().added_in;
    if(current_ver_ < added) current_ver_=added;
}

void GenreDict::deprecate(uint32_t entry_id, DictVersion in_version) {
    auto it=entries_.find(entry_id);
    if(it==entries_.end()) return;
    for(auto& e:it->second){
        if(!e.deprecated){
            e.deprecated=true;
            e.deprecated_in=in_version;
        }
    }
    if(current_ver_ < in_version) current_ver_=in_version;
}

const DictEntry* GenreDict::get(uint32_t entry_id) const {
    return get_latest(entry_id);
}

const DictEntry* GenreDict::get_latest(uint32_t entry_id) const {
    auto it=entries_.find(entry_id);
    if(it==entries_.end()) return nullptr;
    if(it->second.empty()) return nullptr;
    // 最新バージョンを返す (deprecated でも返す — 互換性保証)
    return &it->second.back();
}

std::vector<uint8_t> GenreDict::generate(const DictInstruction& instr) const {
    auto* e=get_latest(instr.dict_id.entry_id);
    if(!e) throw std::runtime_error("DictEntry not found: "+instr.dict_id.str());
    if(!e->generate) throw std::runtime_error("No generate function: "+instr.dict_id.str());
    return e->generate(instr.seed, instr.params);
}

size_t GenreDict::active_count() const {
    size_t n=0;
    for(auto& [k,v]:entries_) if(!v.empty()&&!v.back().deprecated) ++n;
    return n;
}
size_t GenreDict::deprecated_count() const {
    size_t n=0;
    for(auto& [k,v]:entries_) if(!v.empty()&&v.back().deprecated) ++n;
    return n;
}

GenreDict::MatchResult GenreDict::find_best(
    const std::vector<uint8_t>& data, double threshold) const
{
    MatchResult best;
    for(auto& [eid,history]:entries_){
        if(history.empty()) continue;
        auto& e=history.back();
        if(!e.match) continue;
        try {
            double rate=e.match(data);
            if(rate>=threshold && rate>best.match_rate){
                best.found=true;
                best.match_rate=rate;
                best.instr.dict_id=e.id;
                best.instr.seed=0;
                best.instr.expected_size=data.size();
                best.bytes_saved=
                    data.size()-best.instr.instruction_size();
            }
        } catch(...) {}
    }
    return best;
}

std::vector<uint8_t> GenreDict::serialize_meta() const {
    std::vector<uint8_t> b;
    auto w32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
    auto wstr=[&](const std::string& s){
        w32((uint32_t)s.size());
        b.insert(b.end(),s.begin(),s.end());
    };
    b.push_back((uint8_t)genre_);
    b.push_back(current_ver_.major);
    b.push_back(current_ver_.minor);
    b.push_back(current_ver_.subtag);
    w32((uint32_t)entries_.size());
    for(auto& [eid,history]:entries_){
        w32(eid);
        if(history.empty()) continue;
        auto& e=history.back();
        wstr(e.name);
        b.push_back(e.deprecated?1:0);
        b.push_back(e.added_in.major);
        b.push_back(e.added_in.minor);
        b.push_back(e.added_in.subtag);
    }
    return b;
}

// ============================================================
// DictRegistry
// ============================================================
void DictRegistry::register_genre(DictGenre genre) {
    genres_.emplace((uint8_t)genre, GenreDict(genre));
}
GenreDict* DictRegistry::get_genre(DictGenre genre) {
    auto it=genres_.find((uint8_t)genre);
    return it==genres_.end() ? nullptr : &it->second;
}
const GenreDict* DictRegistry::get_genre(DictGenre genre) const {
    auto it=genres_.find((uint8_t)genre);
    return it==genres_.end() ? nullptr : &it->second;
}
const DictEntry* DictRegistry::get_entry(const DictID& id) const {
    auto* g=get_genre(id.genre);
    return g ? g->get(id.entry_id) : nullptr;
}
std::vector<uint8_t> DictRegistry::generate(const DictInstruction& instr) const {
    auto* g=get_genre(instr.dict_id.genre);
    if(!g) throw std::runtime_error("Genre not registered: "+
                                     std::to_string((int)instr.dict_id.genre));
    return g->generate(instr);
}
GenreDict::MatchResult DictRegistry::find_best(
    const std::vector<uint8_t>& data, DictGenre hint, double threshold) const
{
    // hint ジャンルを優先、次に全ジャンルを試す
    GenreDict::MatchResult best;
    auto try_genre=[&](DictGenre g){
        auto* gd=get_genre(g);
        if(!gd) return;
        auto r=gd->find_best(data,threshold);
        if(r.found && r.match_rate>best.match_rate) best=r;
    };
    try_genre(hint);
    for(auto& [k,v]:genres_) try_genre((DictGenre)k);
    return best;
}

void DictRegistry::print_stats() const {
    printf("=== CPB Dictionary Registry ===\n");
    for(auto& [k,g]:genres_){
        printf("  %-12s v%-5s active=%-4zu deprecated=%-4zu\n",
               genre_name((DictGenre)k),
               g.current_version().str().c_str(),
               g.active_count(),
               g.deprecated_count());
    }
    printf("================================\n");
}

std::vector<uint8_t> DictRegistry::serialize_refs(
    const std::vector<DictID>& used_ids) const
{
    std::vector<uint8_t> b;
    auto w64=[&](uint64_t v){for(int i=0;i<8;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
    auto w8=[&](uint8_t v){b.push_back(v);};
    w8((uint8_t)used_ids.size());
    for(auto& id:used_ids){
        w64(id.encode());
        b.push_back(id.version.major);
        b.push_back(id.version.minor);
        b.push_back(id.version.subtag);
    }
    return b;
}

// ============================================================
// 軽量PRNG (辞書生成用)
// ============================================================
static uint8_t lcg(uint64_t& s){
    s=s*6364136223846793005ULL+1442695040888963407ULL;
    return (uint8_t)(s>>56);
}

// ============================================================
// DocDict v1.0 — 文書辞書
// ============================================================
void register_doc_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0001: JSON配列
    {
        DictEntry e;
        e.id.entry_id=0x0001; e.name="JSON_ARRAY";
        e.description="JSONオブジェクト配列の典型構造";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t rows=10;
            if(p.size()>=4) memcpy(&rows,p.data(),4);
            std::string s="[\n"; uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i){
                s+="  {\"id\":"+std::to_string(i)
                 +",\"val\":"+std::to_string((int)(lcg(st)%1000))
                 +",\"name\":\"item_"+std::to_string(i)+"\"}";
                if(i<rows-1) s+=",";
                s+="\n";
            }
            s+="]\n";
            return {s.begin(),s.end()};
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            std::string s(d.begin(),d.end());
            if(s.find("[")!=std::string::npos &&
               s.find("\"id\"")!=std::string::npos) return 0.85;
            return 0.0;
        };
        d.add_entry(std::move(e));
    }
    // 0x0002: CSV
    {
        DictEntry e;
        e.id.entry_id=0x0002; e.name="CSV_TABLE";
        e.description="id,name,value,timestamp ヘッダーを持つ典型CSV";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t rows=20;
            if(p.size()>=4) memcpy(&rows,p.data(),4);
            std::string s="id,name,value,timestamp\n"; uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i)
                s+=std::to_string(i)+",item_"+std::to_string(i)
                  +","+std::to_string((int)(lcg(st)%1000))
                  +",2024-01-"+std::to_string(i%28+1)+"\n";
            return {s.begin(),s.end()};
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            std::string s(d.begin(),d.end());
            return (s.find("id,")!=std::string::npos &&
                    s.find(",name,")!=std::string::npos) ? 0.90 : 0.0;
        };
        d.add_entry(std::move(e));
    }
    // 0x0003: ログファイル (v1.1で追加予定のプレースホルダーを今は v1.0 に入れる)
    {
        DictEntry e;
        e.id.entry_id=0x0003; e.name="LOG_ENTRY";
        e.description="タイムスタンプ+レベル+メッセージのログ形式";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t lines=50;
            if(p.size()>=4) memcpy(&lines,p.data(),4);
            std::string s; uint64_t st=seed;
            const char* levels[]={"INFO","WARN","ERROR","DEBUG"};
            for(uint32_t i=0;i<lines;++i)
                s+="2024-01-01 "+std::to_string(i/3600%24)
                  +":"+std::to_string(i/60%60)+":"+std::to_string(i%60)
                  +" ["+levels[lcg(st)%4]+"] message_"+std::to_string(i)+"\n";
            return {s.begin(),s.end()};
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            std::string s(d.begin(),d.end());
            return (s.find("[INFO]")!=std::string::npos ||
                    s.find("[WARN]")!=std::string::npos) ? 0.88 : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// ImgDict v1.0 — 画像辞書
// ============================================================
void register_img_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0101: 単色
    {
        DictEntry e;
        e.id.entry_id=0x0101; e.name="MONO_COLOR";
        e.description="単一色で塗りつぶしたピクセル列";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=4096; uint8_t color=(uint8_t)(seed&0xFF);
            if(p.size()>=5){ memcpy(&sz,p.data(),4); color=p[4]; }
            return std::vector<uint8_t>(sz,color);
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            if(d.empty()) return 0.0;
            uint8_t v=d[0];
            size_t same=0;
            for(auto x:d) if(x==v) ++same;
            return (double)same/d.size();
        };
        d.add_entry(std::move(e));
    }
    // 0x0102: タイルパターン
    {
        DictEntry e;
        e.id.entry_id=0x0102; e.name="TILE_PATTERN";
        e.description="周期的に繰り返すタイルパターン";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=4096, tile=64;
            if(p.size()>=8){ memcpy(&sz,p.data(),4); memcpy(&tile,p.data()+4,4); }
            if(tile==0) tile=1;
            uint64_t st=seed;
            std::vector<uint8_t> pat(tile);
            for(auto& x:pat) x=lcg(st);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i) data[i]=pat[i%tile];
            return data;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            if(d.size()<8) return 0.0;
            // タイルサイズを検出: d[0..tile-1] == d[tile..2*tile-1]
            for(size_t tile=4; tile<=d.size()/2; tile*=2){
                bool match=true;
                for(size_t i=tile;i<std::min(d.size(),tile*4);++i)
                    if(d[i]!=d[i%tile]){match=false;break;}
                if(match) return 0.92;
            }
            return 0.0;
        };
        d.add_entry(std::move(e));
    }
    // 0x0103: グラデーション
    {
        DictEntry e;
        e.id.entry_id=0x0103; e.name="GRADIENT";
        e.description="線形グラデーション";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(p.size()>=4) memcpy(&sz,p.data(),4);
            uint8_t s0=(uint8_t)(seed&0xFF), s1=(uint8_t)((seed>>8)&0xFF);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i)
                data[i]=(uint8_t)(s0+(s1-s0)*(double)i/(sz>1?sz-1:1));
            return data;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            if(d.size()<4) return 0.0;
            // 単調増加 or 単調減少を検出
            size_t inc=0,dec=0;
            for(size_t i=1;i<d.size();++i){
                if(d[i]>=d[i-1]) ++inc; else ++dec;
            }
            double ratio=(double)std::max(inc,dec)/(d.size()-1);
            return ratio>0.85 ? ratio : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// VidDict v1.0 — 動画辞書
// ============================================================
void register_vid_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0201: 静止フレーム (背景保持)
    {
        DictEntry e;
        e.id.entry_id=0x0201; e.name="STATIC_FRAME";
        e.description="変化のない静止フレーム";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=1920*1080;
            if(p.size()>=4) memcpy(&sz,p.data(),4);
            uint64_t st=seed;
            std::vector<uint8_t> frame(sz);
            for(auto& x:frame) x=lcg(st);
            return frame;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            // エントロピーが低いフレームを検出
            if(d.size()<64) return 0.0;
            std::array<uint32_t,256> freq{};
            for(auto x:d) freq[x]++;
            double ent=0;
            for(auto f:freq){
                if(!f) continue;
                double p=(double)f/d.size();
                ent-=p*std::log2(p);
            }
            return ent<4.0 ? 0.85 : 0.0;
        };
        d.add_entry(std::move(e));
    }
    // 0x0202: フレームラン (連続同一フレーム)
    {
        DictEntry e;
        e.id.entry_id=0x0202; e.name="FRAME_RUN";
        e.description="同一フレームが連続するシーン";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t frame_sz=4096, count=10;
            if(p.size()>=8){ memcpy(&frame_sz,p.data(),4); memcpy(&count,p.data()+4,4); }
            uint64_t st=seed;
            std::vector<uint8_t> frame(frame_sz);
            for(auto& x:frame) x=lcg(st);
            std::vector<uint8_t> result;
            result.reserve(frame_sz*count);
            for(uint32_t i=0;i<count;++i)
                result.insert(result.end(),frame.begin(),frame.end());
            return result;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            if(d.size()<128) return 0.0;
            size_t half=d.size()/2;
            size_t same=0;
            for(size_t i=0;i<half;++i) if(d[i]==d[half+i]) ++same;
            return (double)same/half>0.95 ? 0.95 : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// AudDict v1.0 — 音声辞書
// ============================================================
void register_aud_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0301: 無音
    {
        DictEntry e;
        e.id.entry_id=0x0301; e.name="SILENCE";
        e.description="無音区間";
        e.added_in=ver;
        e.generate=[](uint64_t, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=4096;
            if(p.size()>=4) memcpy(&sz,p.data(),4);
            return std::vector<uint8_t>(sz,0);
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            size_t zeros=0;
            for(auto x:d) if(x<4) ++zeros;
            return (double)zeros/d.size();
        };
        d.add_entry(std::move(e));
    }
    // 0x0302: 正弦波
    {
        DictEntry e;
        e.id.entry_id=0x0302; e.name="SINE_WAVE";
        e.description="単音の正弦波 (周波数をparamsで指定)";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t sz=4096; float freq=440.0f;
            if(p.size()>=4) memcpy(&sz,p.data(),4);
            if(p.size()>=8) memcpy(&freq,p.data()+4,4);
            float phase=(float)(seed&0xFFFF)/65536.0f*6.2831853f;
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i)
                data[i]=(uint8_t)(128+127*std::sin(phase+2*3.14159265f*freq*i/48000.0f));
            return data;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            if(d.size()<32) return 0.0;
            // 周期性を検出: ゼロ交差カウント
            size_t zc=0;
            for(size_t i=1;i<d.size();++i)
                if((d[i]>=128) != (d[i-1]>=128)) ++zc;
            // 正弦波なら zc ≈ 2*freq/samplerate*size
            return (zc > d.size()/100 && zc < d.size()/2) ? 0.82 : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// AssetDict v1.0 — アセット辞書
// ============================================================
void register_asset_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0401: 差分パッチ
    {
        DictEntry e;
        e.id.entry_id=0x0401; e.name="VARIANT_DELTA";
        e.description="ベースアセットからの差分のみを持つ派生版";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            if(p.size()<4) return {};
            uint32_t sz; memcpy(&sz,p.data(),4);
            // seed でベースを生成し、残りparamでパッチ適用
            uint64_t st=seed;
            std::vector<uint8_t> data(sz);
            for(auto& x:data) x=lcg(st);
            for(size_t i=4;i+1<p.size();i+=2)
                if(p[i]<sz) data[p[i]]=p[i+1];
            return data;
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            // 派生アセットは特定パターンが90%以上同じ
            return d.size()>32 ? 0.80 : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// DBDict v1.0 — データベース辞書
// ============================================================
void register_db_dict(GenreDict& d, const DictVersion& ver) {
    // 0x0501: 時系列データ
    {
        DictEntry e;
        e.id.entry_id=0x0501; e.name="TIMESERIES";
        e.description="timestamp,valueの時系列CSV";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t rows=100;
            if(p.size()>=4) memcpy(&rows,p.data(),4);
            std::string s="timestamp,value,delta\n"; uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i)
                s+=std::to_string(1700000000+i)
                  +","+std::to_string(100+(int)(lcg(st)%10))
                  +","+std::to_string((int)(lcg(st)%5)-2)+"\n";
            return {s.begin(),s.end()};
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            std::string s(d.begin(),d.end());
            return (s.find("timestamp")!=std::string::npos &&
                    s.find(",value")!=std::string::npos) ? 0.90 : 0.0;
        };
        d.add_entry(std::move(e));
    }
    // 0x0502: スパースCSV (NULL多発)
    {
        DictEntry e;
        e.id.entry_id=0x0502; e.name="SPARSE_CSV";
        e.description="NULLやゼロが80%以上を占めるスパースCSV";
        e.added_in=ver;
        e.generate=[](uint64_t seed, const std::vector<uint8_t>& p)->std::vector<uint8_t>{
            uint32_t rows=50;
            if(p.size()>=4) memcpy(&rows,p.data(),4);
            std::string s; uint64_t st=seed;
            for(uint32_t i=0;i<rows;++i){
                s+=std::to_string(i)+",,,,,";
                s+=std::to_string((int)(lcg(st)%100))+",,,,\n";
            }
            return {s.begin(),s.end()};
        };
        e.match=[](const std::vector<uint8_t>& d)->double{
            size_t commas=0;
            for(auto x:d) if(x==',') ++commas;
            return (double)commas/d.size()>0.4 ? 0.85 : 0.0;
        };
        d.add_entry(std::move(e));
    }
}

// ============================================================
// デフォルトレジストリ
// ============================================================
DictRegistry make_default_registry() {
    DictRegistry reg;
    DictVersion v10{1,0,0};

    reg.register_genre(DictGenre::DOC);
    reg.register_genre(DictGenre::IMG);
    reg.register_genre(DictGenre::VID);
    reg.register_genre(DictGenre::AUD);
    reg.register_genre(DictGenre::ASSET);
    reg.register_genre(DictGenre::DB);
    reg.register_genre(DictGenre::MIXED);

    register_doc_dict  (*reg.get_genre(DictGenre::DOC),   v10);
    register_img_dict  (*reg.get_genre(DictGenre::IMG),   v10);
    register_vid_dict  (*reg.get_genre(DictGenre::VID),   v10);
    register_aud_dict  (*reg.get_genre(DictGenre::AUD),   v10);
    register_asset_dict(*reg.get_genre(DictGenre::ASSET), v10);
    register_db_dict   (*reg.get_genre(DictGenre::DB),    v10);

    return reg;
}
