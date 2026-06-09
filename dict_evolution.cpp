// dict_evolution.cpp — CPB Dictionary Evolution Protocol 実装
#include "dict_evolution.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <sstream>

static uint64_t now_us(){
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static uint8_t lcg8(uint64_t& s){
    s=s*6364136223846793005ULL+1442695040888963407ULL; return(uint8_t)(s>>56);
}

// ============================================================
// Step 1: パターン検出
// ============================================================

Observation DictEvolution::detect_mono(const std::vector<uint8_t>& d) const {
    if(d.empty()) return {};
    uint8_t v=d[0]; size_t same=0;
    for(auto x:d) if(x==v) ++same;
    double strength=(double)same/d.size();
    if(strength<0.90) return {};
    Observation o;
    o.type=PatternType::MONO_COLOR;
    o.genre=(v==0)?DictGenre::AUD:DictGenre::IMG;
    o.strength=strength;
    o.data_size=d.size();
    // params: [size:4][color:1]
    o.params.resize(5);
    uint32_t sz=(uint32_t)d.size();
    memcpy(o.params.data(),&sz,4); o.params[4]=v;
    char buf[64]; snprintf(buf,sizeof(buf),"単色 0x%02X (%.1f%%)",v,strength*100);
    o.detail=buf;
    return o;
}

Observation DictEvolution::detect_tile(const std::vector<uint8_t>& d) const {
    if(d.size()<16) return {};
    for(size_t tile=4; tile<=d.size()/2; tile*=2){
        size_t match=0, total=0;
        for(size_t i=tile; i<std::min(d.size(),tile*8); ++i,++total)
            if(d[i]==d[i%tile]) ++match;
        if(total==0) continue;
        double strength=(double)match/total;
        if(strength>=0.95){
            Observation o;
            o.type=PatternType::TILE_REPEAT;
            o.genre=DictGenre::IMG;
            o.strength=strength;
            o.data_size=d.size();
            o.params.resize(8);
            uint32_t sz=(uint32_t)d.size(), tsz=(uint32_t)tile;
            memcpy(o.params.data(),&sz,4); memcpy(o.params.data()+4,&tsz,4);
            char buf[64]; snprintf(buf,sizeof(buf),"タイル%zuB周期 (%.1f%%)",tile,strength*100);
            o.detail=buf;
            return o;
        }
    }
    return {};
}

Observation DictEvolution::detect_gradient(const std::vector<uint8_t>& d) const {
    if(d.size()<8) return {};
    size_t nondec=0, noninc=0;
    for(size_t i=1;i<d.size();++i){
        if(d[i]>=d[i-1]) ++nondec;
        if(d[i]<=d[i-1]) ++noninc;
    }
    size_t total=d.size()-1;
    double strength=(double)std::max(nondec,noninc)/total;
    if(strength<0.95) return {};
    // 純粋フラット (全部等値) はMONO_COLORに任せる
    if(d.front()==d.back()) return {};
    Observation o;
    o.type=PatternType::GRADIENT;
    o.genre=DictGenre::IMG;
    o.strength=strength;
    o.data_size=d.size();
    o.params.resize(4);
    uint32_t sz=(uint32_t)d.size(); memcpy(o.params.data(),&sz,4);
    char buf[64]; snprintf(buf,sizeof(buf),"%s グラデーション (%.1f%%)",
        nondec>noninc?"単調増加":"単調減少",strength*100);
    o.detail=buf;
    return o;
}

Observation DictEvolution::detect_silence(const std::vector<uint8_t>& d) const {
    if(d.empty()) return {};
    size_t silent=0;
    for(auto x:d) if(x<4) ++silent;
    double strength=(double)silent/d.size();
    if(strength<0.85) return {};
    Observation o;
    o.type=PatternType::SILENCE;
    o.genre=DictGenre::AUD;
    o.strength=strength;
    o.data_size=d.size();
    o.params.resize(4);
    uint32_t sz=(uint32_t)d.size(); memcpy(o.params.data(),&sz,4);
    char buf[64]; snprintf(buf,sizeof(buf),"無音区間 (%.1f%%)",strength*100);
    o.detail=buf;
    return o;
}

Observation DictEvolution::detect_sine(const std::vector<uint8_t>& d) const {
    if(d.size()<32) return {};
    size_t zc=0;
    for(size_t i=1;i<d.size();++i)
        if((d[i]>=128)!=(d[i-1]>=128)) ++zc;
    bool periodic=(zc>d.size()/200 && zc<d.size()/4);
    // 平均が128付近かどうか
    double mean=0; for(auto x:d) mean+=x; mean/=d.size();
    bool centered=(mean>100 && mean<156);
    if(!periodic||!centered) return {};
    Observation o;
    o.type=PatternType::SINE_LIKE;
    o.genre=DictGenre::AUD;
    o.strength=0.82;
    o.data_size=d.size();
    o.params.resize(4);
    uint32_t sz=(uint32_t)d.size(); memcpy(o.params.data(),&sz,4);
    char buf[64]; snprintf(buf,sizeof(buf),"正弦波様 ZC=%zu mean=%.1f",zc,mean);
    o.detail=buf;
    return o;
}

Observation DictEvolution::detect_frame_run(const std::vector<uint8_t>& d) const {
    if(d.size()<128) return {};
    size_t half=d.size()/2;
    size_t same=0;
    for(size_t i=0;i<half;++i) if(d[i]==d[half+i]) ++same;
    double strength=(double)same/half;
    if(strength<0.95) return {};
    Observation o;
    o.type=PatternType::FRAME_RUN;
    o.genre=DictGenre::VID;
    o.strength=strength;
    o.data_size=d.size();
    char buf[64]; snprintf(buf,sizeof(buf),"フレーム反復 (%.1f%%)",strength*100);
    o.detail=buf;
    return o;
}

Observation DictEvolution::detect_doc(const std::vector<uint8_t>& d) const {
    if(d.size()<8) return {};
    std::string s(d.begin(),d.begin()+std::min(d.size(),size_t(512)));
    if(s.find("[")!=std::string::npos && s.find("\"id\"")!=std::string::npos){
        Observation o;
        o.type=PatternType::JSON_STRUCT; o.genre=DictGenre::DOC;
        o.strength=0.85; o.data_size=d.size();
        o.detail="JSON配列構造検出"; return o;
    }
    if(s.find("id,")!=std::string::npos && s.find(",name,")!=std::string::npos){
        Observation o;
        o.type=PatternType::CSV_TABLE; o.genre=DictGenre::DOC;
        o.strength=0.88; o.data_size=d.size();
        o.detail="CSV表形式検出"; return o;
    }
    size_t commas=0; for(auto x:d) if(x==',') ++commas;
    if((double)commas/d.size()>0.35){
        Observation o;
        o.type=PatternType::SPARSE_CSV; o.genre=DictGenre::DB;
        o.strength=0.80; o.data_size=d.size();
        o.detail="スパースCSV検出"; return o;
    }
    if(s.find("[INFO]")!=std::string::npos||s.find("[WARN]")!=std::string::npos){
        Observation o;
        o.type=PatternType::LOG_FORMAT; o.genre=DictGenre::DOC;
        o.strength=0.88; o.data_size=d.size();
        o.detail="ログフォーマット検出"; return o;
    }
    return {};
}

// ============================================================
// 観測メイン
// ============================================================
Observation DictEvolution::observe(
    const std::vector<uint8_t>& data, DictGenre) const
{
    auto all = observe_all(data);
    if(all.empty()) return {};
    return all[0];
}

std::vector<Observation> DictEvolution::observe_all(
    const std::vector<uint8_t>& data) const
{
    std::vector<Observation> results;
    auto add=[&](Observation o){ if(o.strength>0.0) results.push_back(o); };

    add(detect_silence(data));   // 音声: 無音
    add(detect_mono(data));      // 画像: 単色
    add(detect_tile(data));      // 画像: タイル
    add(detect_frame_run(data)); // 動画: フレーム反復
    add(detect_gradient(data));  // 画像: グラデーション
    add(detect_sine(data));      // 音声: 正弦波
    add(detect_doc(data));       // 文書: JSON/CSV/Log

    // 強い順にソート
    std::sort(results.begin(),results.end(),
              [](auto& a,auto& b){ return a.strength>b.strength; });
    return results;
}

// ============================================================
// Step 2: 提案
// ============================================================
std::optional<Proposal> DictEvolution::propose(
    const Observation& obs, double novelty_threshold)
{
    if(obs.strength==0.0) return std::nullopt;

    // 既存辞書でカバーできるか確認
    auto* g=registry_.get_genre(obs.genre);
    if(g){
        // 既存エントリとの最高一致率を計算
        // (ここでは観測のstrengthと比較)
        if(obs.strength>=novelty_threshold){
            // 既存辞書で十分カバーできる → 提案不要
            // ただし一致率が novelty_threshold 未満なら新規として提案
        }
    }

    Proposal p;
    p.id=next_id_++;
    p.obs=obs;
    p.status=ProposalStatus::PENDING;
    p.match_rate=obs.strength;
    p.timestamp=now_us();
    p.target_genre=obs.genre;

    // 名前の自動提案
    char buf[64];
    snprintf(buf,sizeof(buf),"%s_v%.0f",
             pattern_name(obs.type), obs.strength*100);
    p.suggested_name=buf;
    p.description=obs.detail+" (size="+std::to_string(obs.data_size)+"B)";

    // 削減バイト数の見積もり: 指示サイズ(28B) vs 元データサイズ
    p.bytes_saved=(obs.data_size>28) ? obs.data_size-28 : 0;

    return p;
}

std::optional<Proposal> DictEvolution::observe_and_propose(
    const std::vector<uint8_t>& data, DictGenre hint)
{
    auto obs=observe(data,hint);
    if(obs.strength==0.0) return std::nullopt;
    return propose(obs);
}

uint32_t DictEvolution::submit(Proposal p) {
    uint32_t id=p.id;
    if(p.id==0) p.id=next_id_++;
    proposals_.push_back(std::move(p));
    return id;
}

// ============================================================
// Step 3: 承認
// ============================================================
DictEntry DictEvolution::build_entry(const Proposal& p) const {
    DictEntry e;
    e.id.genre     = p.target_genre;
    e.id.entry_id  = p.assigned_entry_id;
    e.id.version   = {1,0,0};
    e.name         = p.suggested_name;
    e.description  = p.description;
    e.deprecated   = false;
    e.added_in     = {1,0,0};

    PatternType pt=p.obs.type;
    const auto& params=p.obs.params;

    e.generate=[pt,params](uint64_t seed,
                            const std::vector<uint8_t>& p2)->std::vector<uint8_t>
    {
        const auto& use_params=p2.empty()?params:p2;
        uint32_t sz=4096;
        if(use_params.size()>=4) memcpy(&sz,use_params.data(),4);

        switch(pt){
        case PatternType::MONO_COLOR:{
            uint8_t color=(use_params.size()>=5)?use_params[4]:(uint8_t)(seed&0xFF);
            return std::vector<uint8_t>(sz,color);
        }
        case PatternType::SILENCE:
            return std::vector<uint8_t>(sz,0);
        case PatternType::TILE_REPEAT:{
            uint32_t tile=64;
            if(use_params.size()>=8) memcpy(&tile,use_params.data()+4,4);
            if(tile==0) tile=1;
            uint64_t st=seed;
            std::vector<uint8_t> pat(tile);
            for(auto& x:pat) x=lcg8(st);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i) data[i]=pat[i%tile];
            return data;
        }
        case PatternType::GRADIENT:{
            uint8_t s0=(uint8_t)(seed&0xFF), s1=(uint8_t)((seed>>8)&0xFF);
            std::vector<uint8_t> data(sz);
            for(size_t i=0;i<sz;++i)
                data[i]=(uint8_t)(s0+(s1-s0)*(double)i/(sz>1?sz-1:1));
            return data;
        }
        case PatternType::FRAME_RUN:{
            uint32_t fsz=sz/2; uint64_t st=seed;
            std::vector<uint8_t> frame(fsz);
            for(auto& x:frame) x=lcg8(st);
            std::vector<uint8_t> data; data.reserve(sz);
            while(data.size()<sz)
                data.insert(data.end(),frame.begin(),frame.end());
            data.resize(sz); return data;
        }
        default:{
            // JSON/CSV/Log → 簡易生成
            uint64_t st=seed;
            std::vector<uint8_t> data(sz);
            for(auto& x:data) x=lcg8(st);
            return data;
        }
        }
    };

    e.match=[pt](const std::vector<uint8_t>& d)->double{
        if(d.empty()) return 0.0;
        switch(pt){
        case PatternType::MONO_COLOR:{
            uint8_t v=d[0]; size_t s=0;
            for(auto x:d) if(x==v) ++s;
            return (double)s/d.size();
        }
        case PatternType::SILENCE:{
            size_t s=0; for(auto x:d) if(x<4) ++s;
            return (double)s/d.size();
        }
        case PatternType::GRADIENT:{
            size_t inc=0; for(size_t i=1;i<d.size();++i) if(d[i]>=d[i-1]) ++inc;
            return (double)inc/(d.size()-1);
        }
        default: return 0.0;
        }
    };

    return e;
}

bool DictEvolution::approve(uint32_t proposal_id,
                             const std::string& name,
                             const std::string& desc)
{
    for(auto& p:proposals_){
        if(p.id!=proposal_id || p.status!=ProposalStatus::PENDING) continue;

        p.status=ProposalStatus::APPROVED;
        p.assigned_entry_id=next_entry_++;
        if(!name.empty()) p.suggested_name=name;
        if(!desc.empty()) p.description=desc;

        // GenreDict に追加 (append-only)
        auto* g=registry_.get_genre(p.target_genre);
        if(!g){
            registry_.register_genre(p.target_genre);
            g=registry_.get_genre(p.target_genre);
        }
        g->add_entry(build_entry(p));
        return true;
    }
    return false;
}

bool DictEvolution::reject(uint32_t proposal_id) {
    for(auto& p:proposals_){
        if(p.id==proposal_id && p.status==ProposalStatus::PENDING){
            p.status=ProposalStatus::REJECTED;
            return true;
        }
    }
    return false;
}

std::vector<const Proposal*> DictEvolution::pending() const {
    std::vector<const Proposal*> r;
    for(auto& p:proposals_) if(p.status==ProposalStatus::PENDING) r.push_back(&p);
    return r;
}
std::vector<const Proposal*> DictEvolution::all() const {
    std::vector<const Proposal*> r;
    for(auto& p:proposals_) r.push_back(&p);
    return r;
}
const Proposal* DictEvolution::get(uint32_t id) const {
    for(auto& p:proposals_) if(p.id==id) return &p;
    return nullptr;
}
size_t DictEvolution::pending_count() const {
    size_t n=0; for(auto& p:proposals_) if(p.status==ProposalStatus::PENDING) ++n;
    return n;
}

// ============================================================
// Layer5Codec
// ============================================================
Layer5Result Layer5Codec::encode(
    const std::vector<uint8_t>& data,
    DictGenre hint,
    double threshold)
{
    Layer5Result r;
    r.orig_size=data.size();
    ++stats_.encode_count;
    stats_.total_bytes_in+=data.size();

    // 既存辞書でマッチを試みる
    auto match=registry_.find_best(data,hint,threshold);
    if(match.found){
        r.used_dict=true;
        r.match_rate=match.match_rate;
        r.instruction=match.instr;
        r.encoded_size=match.instr.instruction_size();
        ++stats_.dict_hit;
        stats_.total_bytes_out+=r.encoded_size;
        stats_.avg_match_rate=
            (stats_.avg_match_rate*(stats_.dict_hit-1)+match.match_rate)
            / stats_.dict_hit;
        return r;
    }

    // マッチなし → 観測して提案
    ++stats_.dict_miss;
    auto proposal=evolution_.observe_and_propose(data,hint);
    if(proposal){
        uint32_t pid=evolution_.submit(*proposal);
        r.proposal_id=pid;
        ++stats_.proposals_made;
    }
    r.encoded_size=data.size(); // 辞書なし → そのまま
    stats_.total_bytes_out+=r.encoded_size;
    return r;
}

std::vector<uint8_t> Layer5Codec::decode(const DictInstruction& instr) {
    return registry_.generate(instr);
}

// ============================================================
// シリアライズ
// ============================================================
std::vector<uint8_t> DictEvolution::serialize_proposals() const {
    std::vector<uint8_t> b;
    auto w32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
    auto w8 =[&](uint8_t  v){b.push_back(v);};
    auto wstr=[&](const std::string& s){ w32((uint32_t)s.size()); b.insert(b.end(),s.begin(),s.end()); };

    w32((uint32_t)proposals_.size());
    for(auto& p:proposals_){
        w32(p.id); w8((uint8_t)p.status);
        w8((uint8_t)p.obs.type); w8((uint8_t)p.obs.genre);
        // strength as fixed-point
        w32((uint32_t)(p.obs.strength*10000));
        w32((uint32_t)p.obs.data_size);
        w32((uint32_t)p.obs.params.size());
        b.insert(b.end(),p.obs.params.begin(),p.obs.params.end());
        wstr(p.suggested_name); wstr(p.description);
        w32(p.assigned_entry_id);
    }
    return b;
}

void DictEvolution::load_proposals(const std::vector<uint8_t>& buf) {
    if(buf.size()<4) return;
    const uint8_t* p=buf.data();
    auto r32=[&]()->uint32_t{ uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t)(*p++)<<(i*8); return v; };
    auto r8 =[&]()->uint8_t { return *p++; };
    auto rstr=[&]()->std::string{ uint32_t n=r32(); std::string s((const char*)p,n); p+=n; return s; };

    uint32_t n=r32();
    for(uint32_t i=0;i<n;++i){
        Proposal pr;
        pr.id=(uint32_t)r32();
        pr.status=(ProposalStatus)r8();
        pr.obs.type=(PatternType)r8();
        pr.obs.genre=(DictGenre)r8();
        pr.obs.strength=r32()/10000.0;
        pr.obs.data_size=r32();
        uint32_t psz=r32();
        pr.obs.params.assign(p,p+psz); p+=psz;
        pr.suggested_name=rstr();
        pr.description=rstr();
        pr.assigned_entry_id=r32();
        if(pr.id>=next_id_) next_id_=pr.id+1;
        proposals_.push_back(std::move(pr));
    }
}
