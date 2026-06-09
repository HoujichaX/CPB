// main.cpp — CPB CLI
// Cocoa Powder Bottle / Invisible Data Architecture
//
// Usage:
//   cpb pack   <input>     <output.raw>  [--profile STANDARD|ARCHIVE|STEGO|AI]
//                                        [--seed <hex>] [--backend raw|atom|ffv1|h264]
//   cpb unpack <container> <output>
//   cpb search <container> <query>       [--label] [--time <start> <end>]
//   cpb stats  <container>
//   cpb help

#include "cpb_helpers.hpp"
#include "cpb_config.hpp"
#include "layer_pipeline.hpp"
#include "container_backend.hpp"
#include "frame_index.hpp"
#include "search_api.hpp"
#include "l5_dict_persist.hpp"
#include "header.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace std::chrono;

// ── ファイルI/O ──────────────────────────────────────────────
static std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if(!f) throw std::runtime_error("Cannot open: " + path);
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

static void write_file(const std::string& path,
                       const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    if(!f) throw std::runtime_error("Cannot write: " + path);
    f.write((const char*)data.data(), data.size());
}

// ── バックエンド選択 ─────────────────────────────────────────
static BackendType parse_backend(const std::string& s)
{
    if(s=="ffv1"||s=="mkv") return BackendType::FFV1_MKV;
    if(s=="h264"||s=="mp4") return BackendType::H264_MP4;
    if(s=="atom")           return BackendType::MP4_ATOM;
    if(s=="png")            return BackendType::PNG_SEQ;
    return BackendType::RAW;
}

static const char* backend_ext(BackendType t)
{
    switch(t){
    case BackendType::FFV1_MKV: return ".mkv";
    case BackendType::H264_MP4: return ".mp4";
    case BackendType::MP4_ATOM: return ".mp4";
    case BackendType::PNG_SEQ:  return "/";
    default:                    return ".raw";
    }
}

// ── プロファイル選択 ─────────────────────────────────────────
static LayerPipeline parse_profile(const std::string& s, uint64_t seed)
{
    if(s=="ARCHIVE" ||s=="archive") return LayerPipeline::archive();
    if(s=="STEGO"   ||s=="stego"  ) return LayerPipeline::stego();
    if(s=="AI"      ||s=="ai"     ||
       s=="AI_PACKET"             ) return LayerPipeline::ai_packet();
    if(s=="DEFENSE" ||s=="defense") return LayerPipeline::defense();
    return LayerPipeline::standard(); // デフォルト
}

// ── SHA-256 hex表示 ──────────────────────────────────────────
static std::string hex8(const uint8_t* p, size_t n)
{
    std::ostringstream ss;
    for(size_t i=0;i<n;++i) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)p[i];
    return ss.str();
}

// ────────────────────────────────────────────────────────────
// cpb pack <input> <output> [options]
// ────────────────────────────────────────────────────────────
static int cmd_pack(int argc, char** argv)
{
    if(argc < 4){
        fprintf(stderr, "Usage: cpb pack <input> <output> "
                "[--profile P] [--seed HEX] [--backend B]\n");
        return 1;
    }
    std::string in_path  = argv[2];
    std::string out_path = argv[3];

    // オプション解析
    std::string profile_name = "STANDARD";
    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    BackendType backend = BackendType::RAW;

    bool learning = false;
    std::string dict_path;
    for(int i=4; i<argc; ++i){
        std::string a = argv[i];
        if(a=="--profile" && i+1<argc) { profile_name=argv[++i]; }
        else if(a=="--seed"&&i+1<argc) {
            seed = std::stoull(argv[++i], nullptr, 16);
        }
        else if(a=="--backend"&&i+1<argc) { backend=parse_backend(argv[++i]); }
        else if(a=="--learning"||a=="--learn") { learning=true; }
        else if(a=="--dict"&&i+1<argc) { dict_path=argv[++i]; }
    }

    // ── 入力読み込み ──
    auto t0 = steady_clock::now();
    printf("[CPB] reading %s ...\n", in_path.c_str());
    std::vector<uint8_t> payload;
    try { payload = read_file(in_path); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }
    printf("[CPB] input:  %zu bytes\n", payload.size());

    // ── パイプライン設定 ──
    CPBConfig cfg = CPBConfig();
    cfg.fourd_seed = seed;
    cfg.learning   = learning;
    auto pipeline  = parse_profile(profile_name, seed);

    printf("[CPB] profile: %s\n", pipeline.describe().c_str());

    // ── L5辞書ロード ──
    if(!dict_path.empty() && learning) {
        if(l5_cache_load(dict_path))
            printf("[CPB] dict loaded: %s (%zu entries)\n",
                   dict_path.c_str(), l5_learn_cache_size());
    }

    // ── エンコード ──
    auto res = run_pipeline_encode(payload, pipeline, cfg);
    if(!res.success){
        fprintf(stderr,"error: encode failed: %s\n", res.error.c_str());
        return 1;
    }

    // ステージサマリー
    for(auto& s : res.ctx.stage_log){
        printf("  %-22s %7zu → %7zu bytes",
               layer_name(s.layer), s.size_before, s.size_after);
        if(s.size_before > 0)
            printf("  (%.2fx)", (double)s.size_after/s.size_before);
        printf("\n");
    }

    // ── バックエンドにフレームとして格納 ──
    auto be = make_backend(backend);
    VideoFrame vf;
    vf.frame_id  = 0;
    vf.pts_sec   = 0.0;
    vf.data      = res.data;
    be->write_frame(vf);

    // SHA-256 計算
    uint8_t hash[32];
    compute_hash(res.data, hash);

    // ── シリアライズ & 書き出し ──
    auto container = be->serialize();
    printf("[CPB] container: %zu bytes\n", container.size());

    try { write_file(out_path, container); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }

    // ── L5辞書セーブ ──
    if(!dict_path.empty() && learning) {
        if(l5_cache_save(dict_path))
            printf("[CPB] dict saved: %s (%zu entries)\n",
                   dict_path.c_str(), l5_learn_cache_size());
    }

    double elapsed = duration<double,std::milli>(steady_clock::now()-t0).count();
    printf("[CPB] output: %s\n", out_path.c_str());
    printf("[CPB] sha256: %s\n", hex8(hash,8).c_str()+"..."[0]==0?"":
           (hex8(hash,8)+"...").c_str());
    printf("[CPB] done in %.1f ms\n", elapsed);
    printf("[CPB] ratio:  %.2f%%\n",
           (double)res.data.size()/payload.size()*100);
    return 0;
}

// ────────────────────────────────────────────────────────────
// cpb unpack <container> <output>
// ────────────────────────────────────────────────────────────
static int cmd_unpack(int argc, char** argv)
{
    if(argc < 4){
        fprintf(stderr, "Usage: cpb unpack <container> <output> "
                "[--profile P] [--seed HEX] [--backend B]\n");
        return 1;
    }
    std::string in_path  = argv[2];
    std::string out_path = argv[3];

    std::string profile_name = "STANDARD";
    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    BackendType backend = BackendType::RAW;

    bool learning = false;
    std::string dict_path;
    for(int i=4;i<argc;++i){
        std::string a=argv[i];
        if(a=="--profile"&&i+1<argc) profile_name=argv[++i];
        else if(a=="--seed"&&i+1<argc) seed=std::stoull(argv[++i],nullptr,16);
        else if(a=="--backend"&&i+1<argc) backend=parse_backend(argv[++i]);
        else if(a=="--learning"||a=="--learn") learning=true;
        else if(a=="--dict"&&i+1<argc) dict_path=argv[++i];
    }

    auto t0 = steady_clock::now();
    printf("[CPB] reading %s ...\n", in_path.c_str());
    std::vector<uint8_t> container;
    try { container = read_file(in_path); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }

    // バックエンドで展開
    auto be = make_backend(backend);
    if(!be->deserialize(container)){
        fprintf(stderr,"error: failed to parse container\n"); return 1;
    }
    if(be->frame_count() == 0){
        fprintf(stderr,"error: no frames in container\n"); return 1;
    }
    auto vf = be->read_frame(0);

    // デコード
    CPBConfig cfg = CPBConfig();
    cfg.fourd_seed = seed;
    cfg.learning   = learning;
    auto pipeline  = parse_profile(profile_name, seed);

    // L5辞書ロード (learning mode)
    if(!dict_path.empty() && learning) {
        if(l5_cache_load(dict_path))
            printf("[CPB] dict loaded: %s (%zu entries)\n",
                   dict_path.c_str(), l5_learn_cache_size());
    }

    auto res = run_pipeline_decode(vf.data, pipeline, cfg);
    if(!res.success){
        fprintf(stderr,"error: decode failed: %s\n", res.error.c_str());
        return 1;
    }

    for(auto& s : res.ctx.stage_log)
        printf("  %-22s %7zu → %7zu bytes\n",
               layer_name(s.layer), s.size_before, s.size_after);

    try { write_file(out_path, res.data); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }

    double elapsed = duration<double,std::milli>(steady_clock::now()-t0).count();
    printf("[CPB] output: %s  (%zu bytes)\n", out_path.c_str(), res.data.size());
    printf("[CPB] done in %.1f ms\n", elapsed);
    return 0;
}

// ────────────────────────────────────────────────────────────
// cpb search <container> <query> [--label] [--time S E]
// ────────────────────────────────────────────────────────────
static int cmd_search(int argc, char** argv)
{
    if(argc < 4){
        fprintf(stderr,"Usage: cpb search <container> <query> "
                "[--label] [--time <start_sec> <end_sec>] [--backend B]\n");
        return 1;
    }
    std::string in_path = argv[2];
    std::string query   = argv[3];
    bool label_mode = false;
    double t_start = -1, t_end = -1;
    BackendType backend = BackendType::RAW;

    for(int i=4;i<argc;++i){
        std::string a=argv[i];
        if(a=="--label") label_mode=true;
        else if(a=="--time"&&i+2<argc){
            t_start=std::stod(argv[++i]); t_end=std::stod(argv[++i]);
        }
        else if(a=="--backend"&&i+1<argc) backend=parse_backend(argv[++i]);
    }

    std::vector<uint8_t> container;
    try { container = read_file(in_path); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }

    auto be = make_backend(backend);
    if(!be->deserialize(container)){
        fprintf(stderr,"error: failed to parse container\n"); return 1;
    }

    // フレームデータをblobs化
    std::vector<std::vector<uint8_t>> blobs;
    for(uint32_t i=0;i<be->frame_count();++i)
        blobs.push_back(be->read_frame(i).data);

    // FIDXをblobs末尾から読み出すか新規ビルド
    auto idx = build_index(blobs, 30.0, {});

    if(t_start >= 0){
        // 時間範囲検索
        auto hits = range_query(idx, t_start, t_end);
        printf("[CPB] time range [%.2f, %.2f]s → %zu frames\n",
               t_start, t_end, hits.size());
        for(auto fid : hits)
            printf("  frame %u  (%.3f s)\n", fid, frame_to_time(idx,fid));
    } else if(label_mode){
        auto hits = search_label(idx, query);
        printf("[CPB] label '%s' → %zu hits\n", query.c_str(), hits.size());
        for(auto fid : hits)
            printf("  frame %u\n", fid);
    } else {
        // フルテキスト検索
        auto hits = search_text(idx, blobs, query);
        printf("[CPB] text '%s' → %zu hits\n", query.c_str(), hits.size());
        for(auto& h : hits)
            printf("  frame %u  offset %zu  len %u\n",
                   h.frame_id, h.byte_offset, h.match_len);
    }
    return 0;
}

// ────────────────────────────────────────────────────────────
// cpb stats <container>
// ────────────────────────────────────────────────────────────
static int cmd_stats(int argc, char** argv)
{
    if(argc < 3){
        fprintf(stderr,"Usage: cpb stats <container> [--backend B]\n");
        return 1;
    }
    std::string in_path = argv[2];
    BackendType backend = BackendType::RAW;
    for(int i=3;i<argc;++i){
        std::string a=argv[i];
        if(a=="--backend"&&i+1<argc) backend=parse_backend(argv[++i]);
    }

    std::vector<uint8_t> container;
    try { container = read_file(in_path); }
    catch(std::exception& e){ fprintf(stderr,"error: %s\n",e.what()); return 1; }

    auto be = make_backend(backend);
    if(!be->deserialize(container)){
        fprintf(stderr,"error: failed to parse container\n"); return 1;
    }

    auto info = be->info();

    printf("\n");
    printf("  ┌──────────────────────────────────────────┐\n");
    printf("  │  CPB Container Stats                     │\n");
    printf("  ├──────────────────────────────────────────┤\n");
    printf("  │  File:     %-30s│\n", in_path.c_str());
    printf("  │  Backend:  %-30s│\n", info.format_name.c_str());
    printf("  │  Lossless: %-30s│\n", info.lossless?"yes":"no (stego)");
    printf("  │  Frames:   %-30u│\n", info.frame_count);
    printf("  │  FPS:      %-30.1f│\n", info.fps);
    printf("  │  File sz:  %-30zu│\n", container.size());
    printf("  ├──────────────────────────────────────────┤\n");

    size_t total_payload = 0;
    for(uint32_t i=0;i<be->frame_count();++i){
        auto vf = be->read_frame(i);
        total_payload += vf.data.size();
        printf("  │  Frame %3u: %zu bytes  pts=%.3fs%*s│\n",
               vf.frame_id, vf.data.size(), vf.pts_sec,
               (int)std::max(0,(int)(14-std::to_string(vf.data.size()).size())),"");
    }

    // SHA-256
    if(be->frame_count() > 0){
        uint8_t hash[32];
        compute_hash(be->read_frame(0).data, hash);
        printf("  │  SHA-256:  %-30s│\n", (hex8(hash,8)+"...").c_str());
    }

    printf("  │  Payload:  %-30zu│\n", total_payload);
    printf("  │  Overhead: %-30s│\n",
           total_payload>0 ?
           (std::to_string((int)((double)(container.size()-total_payload)/
                                  total_payload*100))+"% container overhead").c_str()
           : "N/A");
    printf("  └──────────────────────────────────────────┘\n");
    printf("\n");
    return 0;
}

// ────────────────────────────────────────────────────────────
// ヘルプ
// ────────────────────────────────────────────────────────────
static void print_help()
{
    printf("\n");
    printf("  CPB — Cocoa Powder Bottle\n");
    printf("  Invisible Data Architecture\n");
    printf("\n");
    printf("  Pack any data as fine as powder. Store it where no one looks.\n");
    printf("\n");
    printf("  COMMANDS\n");
    printf("    cpb pack   <input> <output>   [options]\n");
    printf("    cpb unpack <container> <out>  [options]\n");
    printf("    cpb search <container> <query>[options]\n");
    printf("    cpb stats  <container>        [options]\n");
    printf("\n");
    printf("  OPTIONS\n");
    printf("    --profile  STANDARD|ARCHIVE|STEGO|DEFENSE|AI  (default: STANDARD)\n");
    printf("    --seed     <hex64>    seed for 4D mapping      (default: DEADBEEFCAFEBABE)\n");
    printf("    --backend  raw|atom|ffv1|h264                  (default: raw)\n");
    printf("    --learning            enable L5 learning mode  (slower, builds dict)\n");
    printf("    --dict     <file>     load/save L5 dict cache (use with --learning)\n");
    printf("    --label    (search) label mode\n");
    printf("    --time     <start_sec> <end_sec>  (search) time range\n");
    printf("\n");
    printf("  PROFILES\n");
    printf("    STANDARD  L5→L2→L1→L4b→FIDX   compression priority\n");
    printf("    ARCHIVE   L1→L4b→FIDX           no compression, max compat\n");
    printf("    STEGO     L2→L1→L4a→L4b         stegano priority\n");
    printf("    DEFENSE   L3→L2→L4a→L1→L4b      shuffle then protect\n");
    printf("    AI        L5→L3→L1→FIDX          AI packet (dict+genre)\n");
    printf("\n");
}

// ────────────────────────────────────────────────────────────
// エントリポイント
// ────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    if(argc < 2){ print_help(); return 0; }

    std::string cmd = argv[1];
    if(cmd=="pack"  ||cmd=="p") return cmd_pack  (argc, argv);
    if(cmd=="unpack"||cmd=="u") return cmd_unpack(argc, argv);
    if(cmd=="search"||cmd=="s") return cmd_search(argc, argv);
    if(cmd=="stats" ||cmd=="i") return cmd_stats (argc, argv);
    if(cmd=="help"  ||cmd=="-h"||cmd=="--help"){ print_help(); return 0; }

    fprintf(stderr,"Unknown command: %s\n", cmd.c_str());
    print_help();
    return 1;
}
