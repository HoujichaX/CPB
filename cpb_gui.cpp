// cpb_gui.cpp — CPB GUI (暖色系 + レイヤー色パイプライン)
// cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO
//    cpb_gui.cpp /Fe:cpb.exe /link /SUBSYSTEM:WINDOWS
//    user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib cabinet.lib

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"cabinet.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <numeric>
namespace fs = std::filesystem;

#include "cpb_helpers.hpp"
#include "cpb_config.hpp"
#include "layer_pipeline.hpp"
#include "container_backend.hpp"
#include "l5_dict_persist.hpp"
#include "container.cpp"
#include "rs_codec.cpp"
#include "header.cpp"
#include "dsl_vm.cpp"
#include "frame_index.cpp"
#include "compress_impl.cpp"
#include "protection_layer.cpp"
#include "genre_dsl.cpp"
#include "genre_dsl_vm.cpp"
#include "fourd_map.cpp"
#include "gen_codec.cpp"
#include "cpb_dict_protocol.cpp"
#include "dict_evolution.cpp"
#include "layer_pipeline.cpp"
#include "search_api.cpp"
#include "carrier.hpp"
#include "carrier.cpp"

// ── カラーパレット (暖色系) ───────────────────────────────────
#define C_BG       RGB(253,246,240)
#define C_BG1      RGB(248,238,226)
#define C_BG2      RGB(241,228,212)
#define C_BG3      RGB(230,214,194)
#define C_BORDER   RGB(210,186,160)
#define C_BORDER2  RGB(185,155,120)
#define C_ACCENT   RGB(176,82,22)
#define C_TEXT     RGB(55,38,24)
#define C_TEXT2    RGB(110,85,58)
#define C_TEXT3    RGB(155,128,98)
#define C_OK       RGB(72,128,76)
#define C_WARN     RGB(168,120,24)
#define C_ERR      RGB(168,56,48)
// レイヤー色 (虹6色)
#define LC_L1  RGB(210, 70, 70)   // L1 Protect    赤
#define LC_L2  RGB(210,130, 30)   // L2 Compress   オレンジ
#define LC_L3  RGB(185,165, 15)   // L3 Genre DSL  黄
#define LC_L4  RGB( 50,150, 70)   // L4 4D Map     緑
#define LC_L5  RGB( 30,150,170)   // L5 GenDict    水色
#define LC_FX  RGB(100, 80,180)   // FIDX Search   青紫
#define LC_IN  RGB(140,110, 75)   // IN/OUT        ブラウン

// ── IDs ──────────────────────────────────────────────────────
#define ID_IN        101
#define ID_OUT       102
#define ID_DICT_L5   103
#define ID_DICT_L3   104
#define ID_LOG       110
#define ID_PROG      111
#define ID_COMBO_DIM 107
#define ID_BTN_PACK  201
#define ID_BTN_UNPACK 202
#define ID_BRIN      203
#define ID_BRDIR     204
#define ID_BROUT     205
#define ID_BRL5      206
#define ID_BRL3      207
#define ID_CLRLOG    208
#define ID_TOG_L1    301
#define ID_TOG_L2    302
#define ID_TOG_L3    303
#define ID_TOG_L4    304
#define ID_TOG_L5    305
#define ID_TOG_FIDX  306
#define ID_PROF_LIST 401
#define ID_CHK_LEARN 410
#define ID_CHK_PERFILE 411
#define ID_COMBO_L2  412
#define ID_COMBO_RS  413
#define ID_COMBO_FMT 414
#define ID_EDIT_SEED 415
#define WM_LOG  (WM_USER+1)
#define WM_PROG (WM_USER+2)
#define WM_DONE (WM_USER+3)

static const uint8_t MULTI_PER[8]={'C','P','B','M','P','E','R','!'};
static const uint8_t META_MAGIC[8]={'C','P','B','M','E','T','A','!'};
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

// ── グローバル ────────────────────────────────────────────────
static HWND gWnd;
static HWND gIn,gOut,gDictL5,gDictL3;
static HWND gLog,gProg;
static HWND gBtnPack,gBtnUnpack;
static HWND gTogL1,gTogL2,gTogL3,gTogL4,gTogL5,gTogFidx;
static HWND gProfList,gChkLearn,gChkPerFile;
static HWND gComboL2,gComboRS,gComboFmt,gComboDim,gEditSeed;
static HWND gSidebar,gMainPanel,gRightPanel;
static HFONT gFontUI,gFontBold,gFontMono,gFontSmall;
static HBRUSH gBrBg,gBrBg1,gBrBg2,gBrBg3,gBrAccent;
static std::atomic<bool> gBusy{false};
static int gPipeStep=0;
static double gLastRatio=0.0;
static size_t gLastOrigSize=0,gLastCompSize=0;

// ── ユーティリティ ─────────────────────────────────────────────
static std::wstring GetT(HWND h){int n=GetWindowTextLengthW(h);if(n<=0)return L"";std::wstring s(n+1,0);GetWindowTextW(h,&s[0],n+1);s.resize(n);return s;}
static bool Chk(HWND h){return SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED;}
static void SetChk(HWND h,bool v){SendMessageW(h,BM_SETCHECK,v?BST_CHECKED:BST_UNCHECKED,0);}
static bool IsTogOn(HWND h){return GetWindowLongPtrW(h,GWLP_USERDATA)!=0;}
static void SetTogOn(HWND h,bool v){SetWindowLongPtrW(h,GWLP_USERDATA,v?1:0);InvalidateRect(h,nullptr,TRUE);}
static std::wstring CleanPath(std::wstring p){
    while(!p.empty()&&(p.front()==L' '||p.front()==L'"'))p.erase(p.begin());
    while(!p.empty()&&(p.back()==L' '||p.back()==L'"'))p.pop_back();
    while(!p.empty()&&p.back()==L'\\')p.pop_back();
    return p;}
static bool IsDir(const std::wstring&p){DWORD a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY);}
static bool IsCloudOnly(const std::wstring&p){DWORD a=GetFileAttributesW(p.c_str());if(a==INVALID_FILE_ATTRIBUTES)return false;if(a&FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)return true;if(a&FILE_ATTRIBUTE_OFFLINE){FILE*f=_wfopen(p.c_str(),L"rb");if(!f)return true;int c=fgetc(f);fclose(f);return c==EOF;}return false;}
static bool ReadFile2(const std::wstring&p,std::vector<uint8_t>&out){if(IsCloudOnly(p))return false;FILE*f=_wfopen(p.c_str(),L"rb");if(!f)return false;fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);out.resize(sz);if(sz>0)fread(out.data(),1,sz,f);fclose(f);return true;}
static bool WriteFile2(const std::wstring&p,const void*d,size_t sz){FILE*f=_wfopen(p.c_str(),L"wb");if(!f)return false;fwrite(d,1,sz,f);fclose(f);return true;}
static std::string ws2s(const std::wstring&w){if(w.empty())return{};int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,nullptr);std::string r(n-1,0);WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,&r[0],n,nullptr,nullptr);return r;}
static std::wstring s2ws(const std::string&s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);std::wstring r(n-1,0);MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&r[0],n);return r;}
static std::string FileNameOnly(const std::wstring& p){
    auto pos=p.rfind(L'\\');
    std::wstring fn=(pos!=std::wstring::npos)?p.substr(pos+1):p;
    return ws2s(fn);
}

static void AppendLog(const std::wstring&t){int n=GetWindowTextLengthW(gLog);SendMessageW(gLog,EM_SETSEL,n,n);SendMessageW(gLog,EM_REPLACESEL,FALSE,(LPARAM)(t+L"\r\n").c_str());}
static void LogAsync(const std::wstring&t){PostMessageW(gWnd,WM_LOG,0,(LPARAM)new std::wstring(t));}
static void ProgAsync(int v){PostMessageW(gWnd,WM_PROG,v,0);}
static void DoneAsync(bool ok,const std::wstring&msg,const std::wstring&path=L""){PostMessageW(gWnd,WM_DONE,0,(LPARAM)new std::wstring((ok?L"OK:":L"ER:")+msg+(path.empty()?L"":L"|"+path)));}
static bool IsBinary(const std::vector<uint8_t>&d){size_t c=std::min(d.size(),(size_t)512);int n=0;for(size_t i=0;i<c;i++)if(d[i]==0)n++;return n>(int)(c/10);}

// GDIヘルパー
static void FillRoundRect(HDC dc,RECT r,int rx,HBRUSH br){HRGN g=CreateRoundRectRgn(r.left,r.top,r.right,r.bottom,rx,rx);FillRgn(dc,g,br);DeleteObject(g);}
static void DrawRoundRect(HDC dc,RECT r,int rx,COLORREF c,int t=1){HPEN p=CreatePen(PS_SOLID,t,c);HPEN op=(HPEN)SelectObject(dc,p);HBRUSH nb=(HBRUSH)GetStockObject(NULL_BRUSH);HBRUSH ob=(HBRUSH)SelectObject(dc,nb);RoundRect(dc,r.left,r.top,r.right,r.bottom,rx,rx);SelectObject(dc,op);SelectObject(dc,ob);DeleteObject(p);}

// レイヤー色ヘルパー
static COLORREF LayerColor(LayerID id){
    switch(id){
    case LayerID::L1:       return LC_L1;
    case LayerID::L2:       return LC_L2;
    case LayerID::L3:       return LC_L3;
    case LayerID::L4A:
    case LayerID::L4B:      return LC_L4;
    case LayerID::L5:
    case LayerID::L5_LEARN: return LC_L5;
    case LayerID::FIDX:     return LC_FX;
    default:                return C_BG3;
    }
}
static COLORREF TogColor(HWND h){
    if(h==gTogL1) return LC_L1; if(h==gTogL2) return LC_L2;
    if(h==gTogL3) return LC_L3; if(h==gTogL4) return LC_L4;
    if(h==gTogL5) return LC_L5; if(h==gTogFidx) return LC_FX;
    return C_ACCENT;
}

// ── メタデータ ────────────────────────────────────────────────
struct CpbMeta{ std::string profile; bool layers[6]; bool learning; std::string dict_name; uint64_t seed; };
static std::vector<uint8_t> MetaToBytes(const CpbMeta& m){
    std::string j="{\"profile\":\""+m.profile+"\",\"layers\":[";
    for(int i=0;i<6;i++){j+=(m.layers[i]?"1":"0");if(i<5)j+=",";}
    j+="],\"learn\":"; j+=(m.learning?"true":"false");
    j+=",\"dict\":\""+m.dict_name+"\",";
    char sb[20]; snprintf(sb,20,"%llX",(unsigned long long)m.seed);
    j+="\"seed\":\""; j+=sb; j+="\"}";
    std::vector<uint8_t> out(j.begin(),j.end());
    uint32_t sz=(uint32_t)out.size();
    for(int i=0;i<4;i++) out.push_back((sz>>(i*8))&0xFF);
    out.insert(out.end(),META_MAGIC,META_MAGIC+8);
    return out;
}
static std::string JsonGet(const std::string& j,const std::string& key){
    auto p=j.find("\""+key+"\":");
    if(p==std::string::npos) return "";
    p+=key.size()+3;
    if(j[p]=='"'){++p;std::string v;while(p<j.size()&&j[p]!='"')v+=j[p++];return v;}
    std::string v;while(p<j.size()&&j[p]!=','&&j[p]!='}')v+=j[p++];return v;
}
static bool ReadMeta(std::vector<uint8_t>& c,CpbMeta& m){
    if(c.size()<12) return false;
    size_t end=c.size();
    if(memcmp(c.data()+end-8,META_MAGIC,8)!=0) return false;
    uint32_t jsz=0; for(int i=0;i<4;i++) jsz|=((uint32_t)c[end-12+i])<<(i*8);
    if(jsz+12>c.size()) return false;
    size_t js=end-12-jsz;
    std::string json(c.begin()+js,c.begin()+js+jsz);
    c.resize(js);
    m.profile=JsonGet(json,"profile"); m.dict_name=JsonGet(json,"dict");
    m.learning=(JsonGet(json,"learn")=="true");
    std::string lstr=JsonGet(json,"layers");
    for(int i=0;i<6&&i<(int)lstr.size();i++) m.layers[i]=(lstr[i]=='1');
    std::string ss=JsonGet(json,"seed");
    m.seed=ss.empty()?0xDEADBEEFCAFEBABEULL:strtoull(ss.c_str(),nullptr,16);
    return true;
}

// ── プロファイル ──────────────────────────────────────────────
// 処理順リスト (パイプライン表示 + BuildPL 両方に使う)
struct Prof{
    const wchar_t* name, *desc;
    bool l1,l2,l3,l4,l5,fidx,learn;
    // 処理順: LayerID 列 (終端=-1)
    // 0=L1,1=L2,2=L3,3=L4B,4=L5,5=L5_LEARN,6=FIDX
    int order[8];
};
static const Prof PROFS[]={
    // order: 実際の処理順 (終端=-1)
    {L"STANDARD",  L"L5→L3→L2→L1→L4→FIDX",
     true,true,true,true,true,true,false,  {4,2,1,0,3,6,-1}},
    {L"ARCHIVE",   L"L1→L4→FIDX",
     true,false,false,true,false,true,false, {0,3,6,-1}},
    {L"STEGO",     L"L2→L1→L4",
     true,true,false,true,false,false,false,  {1,0,3,-1}},
    {L"DEFENSE",   L"L3→L2→L4→L1→FIDX",
     true,true,true,true,false,true,false,   {2,1,3,0,6,-1}},
    {L"AI_PACKET", L"L5→L3→L1→FIDX",
     true,false,true,false,true,true,false,   {4,2,0,6,-1}},
    {L"LEARN",     L"L5→L3→L2→L1→L4→FIDX 学習",
     true,true,true,true,true,true,true,     {5,2,1,0,3,6,-1}},
    {L"CUSTOM",    L"個別設定",
     false,false,false,false,false,false,false,{-1}},
};
static const int NPROFS=7;
static void ApplyProf(int i){
    if(i<0||i>=NPROFS-1) return;
    const auto& p=PROFS[i];
    SetTogOn(gTogL1,p.l1); SetTogOn(gTogL2,p.l2); SetTogOn(gTogL3,p.l3);
    SetTogOn(gTogL4,p.l4); SetTogOn(gTogL5,p.l5); SetTogOn(gTogFidx,p.fidx);
    SetChk(gChkLearn,p.learn);
    if(p.learn) SetChk(gChkPerFile,true);
    // トグルとパイプラインを即更新
    if(gSidebar)   InvalidateRect(gSidebar,nullptr,TRUE);
    if(gMainPanel) InvalidateRect(gMainPanel,nullptr,FALSE);
}

// ── パイプライン構築 ──────────────────────────────────────────
// order番号 → LayerID変換
static LayerID OrderToLayerID(int o, bool learn){
    switch(o){
    case 0: return LayerID::L1;
    case 1: return LayerID::L2;
    case 2: return LayerID::L3;
    case 3: return LayerID::L4B;
    case 4: return learn?LayerID::L5_LEARN:LayerID::L5;
    case 5: return LayerID::L5_LEARN;
    case 6: return LayerID::FIDX;
    default:return LayerID::L2;
    }
}
// order番号 → 表示名と色
struct LayerInfo{ const wchar_t* name; COLORREF col; HWND* tog; };
static LayerInfo OrderToInfo(int o){
    switch(o){
    case 0: return {L"L1", LC_L1, &gTogL1};
    case 1: return {L"L2", LC_L2, &gTogL2};
    case 2: return {L"L3", LC_L3, &gTogL3};
    case 3: return {L"L4", LC_L4, &gTogL4};
    case 4: return {L"L5", LC_L5, &gTogL5};
    case 5: return {L"L5", LC_L5, &gTogL5}; // LEARN
    case 6: return {L"IDX",LC_FX, &gTogFidx};
    default:return {L"?",  C_BG3, nullptr};
    }
}

static LayerPipeline BuildPL(){
    int sel=(int)SendMessageW(gProfList,LB_GETCURSEL,0,0);
    bool learn=Chk(gChkLearn);
    std::vector<LayerID> ord;

    if(sel>=0 && sel<NPROFS-1){
        // プロファイルの処理順に従ってレイヤーを積む
        const auto& p=PROFS[sel];
        for(int i=0;i<8&&p.order[i]>=0;i++){
            auto info=OrderToInfo(p.order[i]);
            // トグルがONのレイヤーだけ追加
            if(info.tog&&*info.tog&&IsTogOn(*info.tog))
                ord.push_back(OrderToLayerID(p.order[i],learn));
        }
    } else {
        // CUSTOM: トグルONのものをSTANDARD順で積む
        if(IsTogOn(gTogL5))   ord.push_back(learn?LayerID::L5_LEARN:LayerID::L5);
        if(IsTogOn(gTogL3))   ord.push_back(LayerID::L3);
        if(IsTogOn(gTogL2))   ord.push_back(LayerID::L2);
        if(IsTogOn(gTogL1))   ord.push_back(LayerID::L1);
        if(IsTogOn(gTogL4))   ord.push_back(LayerID::L4B);
        if(IsTogOn(gTogFidx)) ord.push_back(LayerID::FIDX);
    }
    if(ord.empty()) return LayerPipeline::standard();
    return LayerPipeline::custom(ord);
}
static LayerPipeline PLfromMeta(const CpbMeta& m){
    std::vector<LayerID> ord;
    if(m.layers[4]) ord.push_back(m.learning?LayerID::L5_LEARN:LayerID::L5);
    if(m.layers[2]) ord.push_back(LayerID::L3);
    if(m.layers[1]) ord.push_back(LayerID::L2);
    if(m.layers[0]) ord.push_back(LayerID::L1);
    if(m.layers[3]) ord.push_back(LayerID::L4B);
    if(m.layers[5]) ord.push_back(LayerID::FIDX);
    if(ord.empty()) return LayerPipeline::standard();
    return LayerPipeline::custom(ord);
}
static CompressMethod GetL2(){
    int i=(int)SendMessageW(gComboL2,CB_GETCURSEL,0,0);
    switch(i){
    case 1:return CompressMethod::LZMS; case 2:return CompressMethod::MSZIP;
    case 3:return CompressMethod::XPRESS_HUFF; case 4:return CompressMethod::XPRESS;
    case 5:return CompressMethod::LZ4; case 6:return CompressMethod::ZSTD;
    default:return CompressMethod::AUTO;
    }
}
static RSParams GetRS(){
    int i=(int)SendMessageW(gComboRS,CB_GETCURSEL,0,0);
    switch(i){
    case 1:return RSParams::max_protection(); case 2:return RSParams::steganographic();
    case 3:return RSParams::lightweight();    case 4:return RSParams::no_rs();
    default:return RSParams::standard();
    }
}

static DimConfig GetDimConfig(){
    int i=(int)SendMessageW(gComboDim,CB_GETCURSEL,0,0);
    switch(i){
    case 1: return DimConfig::dim8();
    case 2: return DimConfig::dim12();
    case 3: return DimConfig::dim16();
    default:return DimConfig::dim4();
    }
}

static CarrierFormat GetFmt(){
    int i=(int)SendMessageW(gComboFmt,CB_GETCURSEL,0,0);
    switch(i){
    case 1:return CarrierFormat::ZIP; case 2:return CarrierFormat::MP4;
    case 3:return CarrierFormat::PDF; case 4:return CarrierFormat::PNG;
    case 5:return CarrierFormat::RAW; default:return CarrierFormat::CPB;
    }
}


// ── ファイル操作 ───────────────────────────────────────────────
static std::wstring BrowseOpen(){
    wchar_t buf[MAX_PATH]={};
    OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=gWnd; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
    o.lpstrFilter=L"全てのファイル\0*.*\0\0"; o.Flags=OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&o)?buf:L"";
}
static std::wstring BrowseSave(const wchar_t* ext=L"cpb"){
    wchar_t buf[MAX_PATH]={};
    OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=gWnd; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
    o.lpstrFilter=L"CPB Files\0*.cpb;*.raw;*.zip;*.mp4;*.pdf;*.png\0全てのファイル\0*.*\0\0";
    o.lpstrDefExt=ext; o.Flags=OFN_OVERWRITEPROMPT;
    return GetSaveFileNameW(&o)?buf:L"";
}
static std::wstring BrowseDictFile(const wchar_t* filter,const wchar_t* ext){
    wchar_t buf[MAX_PATH]={};
    OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=gWnd; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
    o.lpstrFilter=filter; o.lpstrDefExt=ext; o.Flags=OFN_CREATEPROMPT;
    return GetOpenFileNameW(&o)?buf:L"";
}
static std::wstring BrowseDir(){
    IFileDialog* pfd=nullptr;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_ALL,IID_IFileOpenDialog,(void**)&pfd))) return L"";
    DWORD opts; pfd->GetOptions(&opts);
    pfd->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    std::wstring res;
    if(SUCCEEDED(pfd->Show(gWnd))){
        IShellItem* item=nullptr;
        if(SUCCEEDED(pfd->GetResult(&item))){
            PWSTR path=nullptr;
            if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){ res=path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    pfd->Release(); return res;
}
static void SetInput(const std::wstring& raw){
    std::wstring p=CleanPath(raw); SetWindowTextW(gIn,p.c_str());
    if(GetT(gOut).empty()){
        std::wstring out=IsDir(p)?(p+L".cpb"):([&]{auto d=p.rfind(L'.');return(d!=std::wstring::npos?p.substr(0,d):p)+L".cpb";}());
        SetWindowTextW(gOut,out.c_str());
    }
}

// ── フォルダ処理 ─────────────────────────────────────────────
struct FileEntry{ std::string rel; std::vector<uint8_t> data; };
static std::vector<FileEntry> CollectFiles(const std::wstring& root,std::wstring& skipped){
    std::vector<FileEntry> files;
    std::error_code ec;
    fs::recursive_directory_iterator it;
    try{ it=fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied,ec); }
    catch(...){ return files; }
    for(;it!=fs::end(it);){
        try{
            if(ec){ec.clear();it.increment(ec);continue;}
            auto& e=*it;
            if(!e.is_regular_file(ec)){it.increment(ec);continue;}
            std::wstring fp=e.path().wstring();
            if(IsCloudOnly(fp)){ skipped+=L"[クラウド] "+e.path().filename().wstring()+L"\n"; it.increment(ec); continue; }
            FileEntry fe;
            if(!ReadFile2(fp,fe.data)){ skipped+=L"[読取失敗] "+e.path().filename().wstring()+L"\n"; it.increment(ec); continue; }
            fe.rel=fs::relative(e.path(),root,ec).u8string();
            for(auto& c:fe.rel) if(c=='\\') c='/';
            files.push_back(std::move(fe));
            it.increment(ec);
        }catch(...){try{it.increment(ec);}catch(...){break;}}
    }
    return files;
}
static std::vector<uint8_t> SerializePer(std::vector<FileEntry>& files,const LayerPipeline& pl,const CPBConfig& cfg){
    std::vector<uint8_t> out;
    out.insert(out.end(),MULTI_PER,MULTI_PER+8);
    uint32_t n=(uint32_t)files.size();
    for(int i=0;i<4;i++) out.push_back((n>>(i*8))&0xFF);
    for(auto& fe:files){
        auto enc=run_pipeline_encode(fe.data,pl,cfg);
        const auto& comp=enc.success?enc.data:fe.data;
        uint32_t plen=(uint32_t)fe.rel.size();
        for(int i=0;i<4;i++) out.push_back((plen>>(i*8))&0xFF);
        out.insert(out.end(),fe.rel.begin(),fe.rel.end());
        uint64_t osz=fe.data.size(); for(int i=0;i<8;i++) out.push_back((uint8_t)((osz>>(i*8))&0xFF));
        uint64_t csz=comp.size(); for(int i=0;i<8;i++) out.push_back((uint8_t)((csz>>(i*8))&0xFF));
        out.insert(out.end(),comp.begin(),comp.end());
    }
    return out;
}
static bool DeserializePer(const std::vector<uint8_t>& data,const std::wstring& outDir,const LayerPipeline& pl,const CPBConfig& cfg,int& count){
    if(data.size()<12||memcmp(data.data(),MULTI_PER,8)!=0) return false;
    const uint8_t* p=data.data()+8;
    uint32_t n=0; for(int i=0;i<4;i++) n|=((uint32_t)p[i])<<(i*8); p+=4;
    std::error_code ec; fs::create_directories(outDir,ec);
    count=0;
    for(uint32_t i=0;i<n;i++){
        if(p+4>data.data()+data.size()) return false;
        uint32_t plen=0; for(int j=0;j<4;j++) plen|=((uint32_t)p[j])<<(j*8); p+=4;
        std::string rel((const char*)p,plen); p+=plen;
        uint64_t osz=0; for(int j=0;j<8;j++) osz|=((uint64_t)p[j])<<(j*8); p+=8;
        uint64_t csz=0; for(int j=0;j<8;j++) csz|=((uint64_t)p[j])<<(j*8); p+=8;
        if(p+csz>data.data()+data.size()) return false;
        std::vector<uint8_t> comp(p,p+csz); p+=csz;
        auto dec=run_pipeline_decode(comp,pl,cfg);
        const auto& fd=dec.success?dec.data:comp;
        int wsz=MultiByteToWideChar(CP_UTF8,0,rel.c_str(),-1,nullptr,0);
        std::wstring relW(wsz>0?wsz-1:0,L'\0');
        if(wsz>1) MultiByteToWideChar(CP_UTF8,0,rel.c_str(),-1,&relW[0],wsz);
        for(auto& c:relW) if(c==L'/') c=L'\\';
        fs::path dst=fs::path(outDir)/relW;
        try{ fs::create_directories(dst.parent_path(),ec); }catch(...){}
        WriteFile2(dst.wstring(),fd.data(),fd.size());
        count++;
    }
    return true;
}

// ── PACK/UNPACK ──────────────────────────────────────────────
struct PackArgs{
    std::wstring inW,outW,dictL5W,dictL3W;
    bool learning,perFile;
    LayerPipeline pl;
    uint64_t seed; CompressMethod cm; RSParams rs;
    CarrierFormat carrier;
    DimConfig dims;
    std::string profName; bool layerFlags[6];
};

static void RunPack(PackArgs a){
  try{
    LogAsync(L""); LogAsync(L"[PACK] 開始  " + a.inW);
    ProgAsync(5);
    CPBConfig cfg;
    cfg.learning=a.learning; cfg.fourd_seed=a.seed;
    cfg.compress_method=static_cast<uint8_t>(a.cm);
    cfg.l3_dict_path=ws2s(a.dictL3W);
    cfg.dims=a.dims; cfg.rs=a.rs;
    if(a.learning&&!a.dictL5W.empty()){
        FILE* tf=_wfopen(a.dictL5W.c_str(),L"rb");
        if(tf){ fclose(tf);
            if(l5_cache_load(a.dictL5W)){
                wchar_t b[64]; swprintf(b,64,L"  L5辞書ロード: %zuエントリ",l5_learn_cache_size()); LogAsync(b);
            }
        } else LogAsync(L"  L5辞書: 新規作成");
    }
    if(!a.dictL3W.empty()) LogAsync(L"  L3辞書: "+a.dictL3W);
    ProgAsync(10);
    std::vector<uint8_t> serialized; size_t totalOrig=0;
    if(IsDir(a.inW)){
        std::wstring sk; auto files=CollectFiles(a.inW,sk);
        if(files.empty()){ DoneAsync(false,L"ファイルが見つかりません"); gBusy=false; return; }
        wchar_t b[128]; swprintf(b,128,L"  %dファイル読み込み",(int)files.size()); LogAsync(b);
        if(!sk.empty()) LogAsync(L"  スキップ: "+sk);
        ProgAsync(20);
        if(a.perFile){
            serialized=SerializePer(files,a.pl,cfg);
            for(auto& fe:files) totalOrig+=fe.data.size();
        } else {
            static const uint8_t MHDR[8]={0x43,0x50,0x42,0x4D,0x55,0x4C,0x54,0x49};
            std::vector<uint8_t> raw; raw.insert(raw.end(),MHDR,MHDR+8);
            uint32_t nn=(uint32_t)files.size(); for(int i=0;i<4;i++) raw.push_back((nn>>(i*8))&0xFF);
            for(auto& fe:files){
                uint32_t plen=(uint32_t)fe.rel.size(); for(int i=0;i<4;i++) raw.push_back((plen>>(i*8))&0xFF);
                raw.insert(raw.end(),fe.rel.begin(),fe.rel.end());
                uint64_t fsz=fe.data.size(); for(int i=0;i<8;i++) raw.push_back((uint8_t)((fsz>>(i*8))&0xFF));
                raw.insert(raw.end(),fe.data.begin(),fe.data.end());
                totalOrig+=fe.data.size();
            }
            auto enc=run_pipeline_encode(raw,a.pl,cfg);
            for(auto& sl:enc.ctx.stage_log){
                swprintf(b,128,L"  %hs: %zu→%zuB",layer_name(sl.layer),sl.size_before,sl.size_after); LogAsync(b);
            }
            serialized=enc.success?enc.data:raw;
        }
    } else {
        if(IsCloudOnly(a.inW)){ DoneAsync(false,L"クラウド専用ファイルです"); gBusy=false; return; }
        std::vector<uint8_t> data;
        if(!ReadFile2(a.inW,data)){ DWORD e=GetLastError(); wchar_t b[128]; swprintf(b,128,L"ファイルを開けません (err=%lu)",e); DoneAsync(false,b); gBusy=false; return; }
        wchar_t buf[64]; swprintf(buf,64,L"  データ: %zuB",data.size()); LogAsync(buf);
        ProgAsync(20); LogAsync(L"  エンコード中...");
        auto enc=run_pipeline_encode(data,a.pl,cfg);
        if(!enc.success){ DoneAsync(false,L"エンコード失敗"); gBusy=false; return; }
        for(auto& sl:enc.ctx.stage_log){
            swprintf(buf,64,L"  %hs: %zu→%zuB",layer_name(sl.layer),sl.size_before,sl.size_after); LogAsync(buf);
        }
        serialized=enc.data; totalOrig=data.size();
    }
    if(a.learning){
        wchar_t b[128]; swprintf(b,128,L"  L5キャッシュ: %zuエントリ",l5_learn_cache_size()); LogAsync(b);
        if(!a.dictL5W.empty()&&l5_cache_save(a.dictL5W)){
            swprintf(b,128,L"  L5辞書保存 ✓ (%zuエントリ)",l5_learn_cache_size()); LogAsync(b);
        }
    }
    ProgAsync(75);
    auto be=make_backend(BackendType::RAW);
    VideoFrame vf; vf.frame_id=0; vf.data=serialized; be->write_frame(vf);
    auto container=be->serialize();
    CpbMeta meta; meta.profile=a.profName; meta.learning=a.learning;
    meta.dict_name=FileNameOnly(a.dictL5W); meta.seed=a.seed;
    for(int i=0;i<6;i++) meta.layers[i]=a.layerFlags[i];
    auto mBytes=MetaToBytes(meta); container.insert(container.end(),mBytes.begin(),mBytes.end());
    auto final_data=(a.carrier==CarrierFormat::CPB||a.carrier==CarrierFormat::RAW)?container:carrier_wrap(container,a.carrier);
    if(!WriteFile2(a.outW,final_data.data(),final_data.size())){ DoneAsync(false,L"保存失敗\n"+a.outW); gBusy=false; return; }
    ProgAsync(100);
    double ratio=(double)final_data.size()/(totalOrig?totalOrig:1)*100.0;
    gLastRatio=ratio; gLastOrigSize=totalOrig; gLastCompSize=final_data.size();
    InvalidateRect(gMainPanel,nullptr,FALSE);
    wchar_t done[128]; swprintf(done,128,L"[PACK] 完了 ✓  %.1f%%",ratio); LogAsync(done);
    DoneAsync(true,done,a.outW);
  }catch(std::exception& ex){ DoneAsync(false,L"例外: "+std::wstring(ex.what(),ex.what()+strlen(ex.what()))); }
  catch(...){ DoneAsync(false,L"予期しないエラー"); }
  gBusy=false;
}

struct UnpackArgs{ std::wstring inW,outW,dictL5W,dictL3W; bool learning; LayerPipeline pl; uint64_t seed; DimConfig dims; };
static void RunUnpack(UnpackArgs a){
  try{
    LogAsync(L""); LogAsync(L"[UNPACK] 開始  "+a.inW); ProgAsync(5);
    std::vector<uint8_t> rawFile;
    if(!ReadFile2(a.inW,rawFile)){ wchar_t b[256]; swprintf(b,256,L"ファイルを開けません\n%s",a.inW.c_str()); DoneAsync(false,b); gBusy=false; return; }
    std::vector<uint8_t> container;
    if(is_cpb_carrier(rawFile)){ LogAsync(L"  キャリア検出 → 取り出し中..."); container=carrier_unwrap(rawFile); }
    else container=rawFile;
    ProgAsync(20);
    CpbMeta meta; meta.seed=a.seed; meta.learning=a.learning;
    bool hasMeta=ReadMeta(container,meta);
    LayerPipeline pl=hasMeta?PLfromMeta(meta):a.pl;
    bool useLearning=hasMeta?meta.learning:a.learning;
    if(hasMeta){ LogAsync(L"  メタ: プロファイル="+std::wstring(meta.profile.begin(),meta.profile.end())); }
    if(useLearning&&!a.dictL5W.empty()&&l5_cache_load(a.dictL5W)){
        wchar_t b[64]; swprintf(b,64,L"  L5辞書ロード: %zuエントリ",l5_learn_cache_size()); LogAsync(b);
    }
    CPBConfig cfg; cfg.learning=useLearning; cfg.fourd_seed=hasMeta?meta.seed:a.seed;
    cfg.l3_dict_path=ws2s(a.dictL3W);
    cfg.dims=a.dims;
    cfg.dims=a.dims;
    auto be=make_backend(BackendType::RAW);
    if(!be->deserialize(container)){ DoneAsync(false,L"コンテナ解析失敗"); gBusy=false; return; }
    auto vf=be->read_frame(0); ProgAsync(40);
    static const uint8_t MHDR2[8]={0x43,0x50,0x42,0x4D,0x55,0x4C,0x54,0x49};
    bool isPerFile=vf.data.size()>=8&&memcmp(vf.data.data(),MULTI_PER,8)==0;
    bool isWhole=!isPerFile&&vf.data.size()>=8&&memcmp(vf.data.data(),MHDR2,8)==0;
    std::wstring finalOut=a.outW;
    wchar_t buf[64];
    if(isPerFile){
        auto d=finalOut.rfind(L'.'); if(d!=std::wstring::npos) finalOut=finalOut.substr(0,d);
        int fc=0;
        if(!DeserializePer(vf.data,finalOut,pl,cfg,fc)){ DoneAsync(false,L"展開失敗"); gBusy=false; return; }
        ProgAsync(100); swprintf(buf,64,L"[UNPACK] 完了 ✓  %dファイル",fc); LogAsync(buf); DoneAsync(true,buf,finalOut);
    } else if(isWhole){
        auto d=finalOut.rfind(L'.'); if(d!=std::wstring::npos) finalOut=finalOut.substr(0,d);
        const uint8_t* p=vf.data.data()+8; uint32_t n=0; for(int i=0;i<4;i++) n|=((uint32_t)p[i])<<(i*8); p+=4;
        std::error_code ec2; fs::create_directories(finalOut,ec2); int fc2=0;
        for(uint32_t i=0;i<n;i++){
            if(p+4>vf.data.data()+vf.data.size()) break;
            uint32_t plen=0; for(int j=0;j<4;j++) plen|=((uint32_t)p[j])<<(j*8); p+=4;
            std::string rel((const char*)p,plen); p+=plen;
            uint64_t fsz=0; for(int j=0;j<8;j++) fsz|=((uint64_t)p[j])<<(j*8); p+=8;
            if(p+fsz>vf.data.data()+vf.data.size()) break;
            int wsz=MultiByteToWideChar(CP_UTF8,0,rel.c_str(),-1,nullptr,0);
            std::wstring relW(wsz>0?wsz-1:0,L'\0');
            if(wsz>1) MultiByteToWideChar(CP_UTF8,0,rel.c_str(),-1,&relW[0],wsz);
            for(auto& c:relW) if(c==L'/') c=L'\\';
            fs::path dst=fs::path(finalOut)/relW;
            try{ fs::create_directories(dst.parent_path(),ec2); }catch(...){}
            WriteFile2(dst.wstring(),p,(size_t)fsz); p+=fsz; fc2++;
        }
        ProgAsync(100); swprintf(buf,64,L"[UNPACK] 完了 ✓  %dファイル",fc2); LogAsync(buf); DoneAsync(true,buf,finalOut);
    } else {
        LogAsync(L"  デコード中...");
        auto dec=run_pipeline_decode(vf.data,pl,cfg);
        for(auto& sl:dec.ctx.stage_log){ swprintf(buf,64,L"  %hs: %zu→%zuB",layer_name(sl.layer),sl.size_before,sl.size_after); LogAsync(buf); }
        ProgAsync(80);
        if(!dec.success){ DoneAsync(false,L"デコード失敗\n"+std::wstring(dec.error.begin(),dec.error.end())); gBusy=false; return; }
        if(!WriteFile2(a.outW,dec.data.data(),dec.data.size())){ DoneAsync(false,L"保存失敗"); gBusy=false; return; }
        ProgAsync(100); swprintf(buf,64,L"[UNPACK] 完了 ✓  %zuB",dec.data.size()); LogAsync(buf); DoneAsync(true,buf,a.outW);
    }
  }catch(std::exception& ex){ DoneAsync(false,L"例外: "+std::wstring(ex.what(),ex.what()+strlen(ex.what()))); }
  catch(...){ DoneAsync(false,L"予期しないエラー"); }
  gBusy=false;
}


// レイヤーIDから色を取得
// トグルハンドルからレイヤー色を取得
// ── カスタムコントロール: トグルボタン ────────────────────────
static LRESULT CALLBACK ToggleProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        bool on=IsTogOn(h);
        COLORREF lc=TogColor(h);

        // 背景: ON=レイヤー色の薄塗り、OFF=通常背景
        if(on){
            BYTE r=GetRValue(lc),g=GetGValue(lc),b=GetBValue(lc);
            COLORREF bgc=RGB((int)(255*0.82+r*0.18),(int)(255*0.82+g*0.18),(int)(255*0.82+b*0.18));
            HBRUSH bb=CreateSolidBrush(bgc); FillRect(dc,&rc,bb); DeleteObject(bb);
            RECT bar={0,2,4,rc.bottom-2};
            HBRUSH lb=CreateSolidBrush(lc); FillRect(dc,&bar,lb); DeleteObject(lb);
            DrawRoundRect(dc,{1,1,rc.right-1,rc.bottom-1},4,lc);
        } else {
            HBRUSH bb=CreateSolidBrush(C_BG1); FillRect(dc,&rc,bb); DeleteObject(bb);
            DrawRoundRect(dc,{1,1,rc.right-1,rc.bottom-1},4,C_BORDER);
        }
        // トグルスイッチ (右端)
        int sw=26,sh=14,sx=rc.right-sw-5,sy=(rc.bottom-sh)/2;
        HBRUSH trackBr=CreateSolidBrush(on?lc:C_BORDER2);
        HRGN rgnT=CreateRoundRectRgn(sx,sy,sx+sw,sy+sh,sh,sh);
        FillRgn(dc,rgnT,trackBr); DeleteObject(rgnT); DeleteObject(trackBr);
        int nob=sh-4,nx=on?(sx+sw-nob-2):(sx+2);
        HBRUSH nobBr=CreateSolidBrush(RGB(255,255,255));
        HRGN rgnN=CreateEllipticRgn(nx,sy+2,nx+nob,sy+2+nob);
        FillRgn(dc,rgnN,nobBr); DeleteObject(rgnN); DeleteObject(nobBr);
        // ラベル
        wchar_t label[32]; GetWindowTextW(h,label,32);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,on?lc:C_TEXT3);
        HFONT of=(HFONT)SelectObject(dc,on?gFontBold:gFontUI);
        RECT lr={8,0,sx-4,rc.bottom};
        DrawTextW(dc,label,-1,&lr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(dc,of);
        EndPaint(h,&ps); return 0;
    }
    case WM_LBUTTONUP:{
        SetTogOn(h,!IsTogOn(h));
        // プロファイルをCUSTOMに切り替えてパイプライン更新
        HWND main=GetParent(GetParent(h)); // gWnd
        if(gProfList) SendMessageW(gProfList,LB_SETCURSEL,NPROFS-1,0);
        if(gSidebar)  InvalidateRect(gSidebar,nullptr,TRUE);
        if(gMainPanel)InvalidateRect(gMainPanel,nullptr,FALSE);
        (void)main;
        return 0;}
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

// ── パイプライン描画 (動的・トグル状態反映) ───────────────
static void DrawPipeline(HDC dc, RECT area){
    // ── プロファイルの処理順でノードリストを構築 ──────────────
    struct PNode{ const wchar_t* name; COLORREF col; };
    std::vector<PNode> nodes;
    nodes.push_back({L"IN", LC_IN});

    int sel=(int)SendMessageW(gProfList,LB_GETCURSEL,0,0);
    bool learn=gChkLearn&&(SendMessageW(gChkLearn,BM_GETCHECK,0,0)==BST_CHECKED);

    if(sel>=0 && sel<NPROFS-1){
        // プロファイルの処理順に従う
        const auto& prof=PROFS[sel];
        for(int i=0;i<8&&prof.order[i]>=0;i++){
            auto info=OrderToInfo(prof.order[i]);
            // トグルがONのレイヤーだけ表示
            if(info.tog && *info.tog && IsTogOn(*info.tog)){
                const wchar_t* nm=(prof.order[i]==5)?L"L5":info.name;
                nodes.push_back({nm, info.col});
            }
        }
    } else {
        // CUSTOM: トグルONをSTANDARD順で
        if(gTogL5&&IsTogOn(gTogL5))   nodes.push_back({learn?L"L5":L"L5", LC_L5});
        if(gTogL3&&IsTogOn(gTogL3))   nodes.push_back({L"L3",  LC_L3});
        if(gTogL2&&IsTogOn(gTogL2))   nodes.push_back({L"L2",  LC_L2});
        if(gTogL1&&IsTogOn(gTogL1))   nodes.push_back({L"L1",  LC_L1});
        if(gTogL4&&IsTogOn(gTogL4))   nodes.push_back({L"L4",  LC_L4});
        if(gTogFidx&&IsTogOn(gTogFidx)) nodes.push_back({L"IDX",LC_FX});
    }
    nodes.push_back({L"OUT", LC_IN});

    int n=(int)nodes.size();
    if(n<2) return;

    // ── 左詰め固定幅レイアウト ──────────────────────────────
    int aH  = area.bottom - area.top;
    int cy  = area.top + aH/2;
    int nW  = 38;  // ノード幅
    int nH  = std::min(aH-8, 26); // ノード高
    int gap = 10;  // ノード間の線の長さ
    int step= gPipeStep;

    for(int i=0; i<n; i++){
        int cx = area.left + nW/2 + (nW+gap)*i;
        if(cx + nW/2 > area.right) break;

        COLORREF lc = nodes[i].col;
        bool done   = (step>0 && i>0 && i<=step && i<n-1);
        bool active = (step>0 && i==step && i<n-1);

        COLORREF bg, bc, tc;
        if(active){
            bg=lc; bc=lc; tc=RGB(255,255,255);
        } else if(done){
            BYTE r=GetRValue(lc),g=GetGValue(lc),b=GetBValue(lc);
            bg=RGB(r*7/10,g*7/10,b*7/10); bc=bg; tc=RGB(255,255,255);
        } else {
            BYTE r=GetRValue(lc),g=GetGValue(lc),b=GetBValue(lc);
            bg=RGB(255*4/5+r/5, 255*4/5+g/5, 255*4/5+b/5);
            bc=lc; tc=lc;
        }

        RECT nr={cx-nW/2, cy-nH/2, cx+nW/2, cy+nH/2};
        HBRUSH nbr=CreateSolidBrush(bg);
        FillRoundRect(dc,nr,5,nbr); DeleteObject(nbr);
        DrawRoundRect(dc,nr,5,bc, done||active?2:1);

        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,tc);
        HFONT of=(HFONT)SelectObject(dc,gFontBold);
        DrawTextW(dc,nodes[i].name,-1,&nr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,of);

        // サブラベル (ノード下)
        if(nH>20 && aH>50){
            const wchar_t* subs[]={L"Input",L"GenDict",L"DSL",L"Compress",
                                    L"Protect",L"4D Map",L"Search",L"Output",L"Learn"};
            // 簡易マッピング
            const wchar_t* sub=L"";
            if(nodes[i].col==LC_IN)  sub=(i==0?L"Input":L"Output");
            else if(nodes[i].col==LC_L1) sub=L"RS";
            else if(nodes[i].col==LC_L2) sub=L"Zip";
            else if(nodes[i].col==LC_L3) sub=L"DSL";
            else if(nodes[i].col==LC_L4) sub=L"4D";
            else if(nodes[i].col==LC_L5) sub=L"Dict";
            else if(nodes[i].col==LC_FX) sub=L"Idx";
            if(sub[0]){
                SetTextColor(dc,done||active?tc:C_TEXT3);
                HFONT of2=(HFONT)SelectObject(dc,gFontSmall);
                RECT sr={cx-nW/2, cy+nH/2+2, cx+nW/2, cy+nH/2+13};
                DrawTextW(dc,sub,-1,&sr,DT_CENTER|DT_TOP|DT_SINGLELINE);
                SelectObject(dc,of2);
            }
        }

        // 接続線 (左詰め・固定長)
        if(i<n-1){
            int lx0=cx+nW/2+1, lx1=cx+nW/2+gap;
            if(lx1<=area.right){
                COLORREF lnc=done?lc:C_BORDER;
                HPEN lp=CreatePen(PS_SOLID,done?2:1,lnc);
                HPEN op=(HPEN)SelectObject(dc,lp);
                MoveToEx(dc,lx0,cy,nullptr);
                LineTo(dc,lx1,cy);
                SelectObject(dc,op); DeleteObject(lp);
            }
        }
    }
}



// ── メトリクスカード描画 ──────────────────────────────────────
static void DrawMetrics(HDC dc,RECT area){
    int cardW=(area.right-area.left-12)/4-3;
    struct Card{ const wchar_t* label; std::wstring val; const wchar_t* sub; COLORREF vc; };
    wchar_t origBuf[32],compBuf[32],ratioBuf[32];
    if(gLastOrigSize>0){
        if(gLastOrigSize>=1024*1024) swprintf(origBuf,32,L"%.1f MB",(double)gLastOrigSize/1024/1024);
        else swprintf(origBuf,32,L"%.1f KB",(double)gLastOrigSize/1024);
        if(gLastCompSize>=1024*1024) swprintf(compBuf,32,L"%.1f MB",(double)gLastCompSize/1024/1024);
        else swprintf(compBuf,32,L"%.1f KB",(double)gLastCompSize/1024);
        swprintf(ratioBuf,32,L"%.1f%%",gLastRatio);
    } else {
        wcscpy(origBuf,L"---"); wcscpy(compBuf,L"---"); wcscpy(ratioBuf,L"--");
    }
    Card cards[]={
        {L"Original",  origBuf, L"入力サイズ",   C_ACCENT},
        {L"Compressed",compBuf, L"出力サイズ",   C_OK},
        {L"Ratio",     ratioBuf,L"圧縮率",       C_WARN},
        {L"Protection",L"RS",   L"Reed-Solomon", RGB(130,65,160)},
    };
    for(int i=0;i<4;i++){
        int x=area.left+(cardW+4)*i;
        RECT cr={x,area.top,x+cardW,area.bottom};
        HBRUSH cbr=CreateSolidBrush(C_BG2); FillRect(dc,&cr,cbr); DeleteObject(cbr);
        DrawRoundRect(dc,cr,6,C_BORDER);
        // ラベル
        RECT lr={cr.left+10,cr.top+8,cr.right-4,cr.top+22};
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT3);
        HFONT of=(HFONT)SelectObject(dc,gFontSmall);
        DrawTextW(dc,cards[i].label,-1,&lr,DT_LEFT|DT_TOP);
        // 値
        RECT vr={cr.left+8,cr.top+20,cr.right-4,cr.bottom-16};
        SetTextColor(dc,cards[i].vc);
        SelectObject(dc,gFontBold);
        DrawTextW(dc,cards[i].val.c_str(),-1,&vr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        // サブ
        RECT sr={cr.left+8,cr.bottom-16,cr.right-4,cr.bottom-2};
        SetTextColor(dc,C_TEXT3);
        SelectObject(dc,of);
        DrawTextW(dc,cards[i].sub,-1,&sr,DT_LEFT|DT_BOTTOM);
        SelectObject(dc,of);
    }
}

// ── メインパネル WndProc ──────────────────────────────────────
static LRESULT CALLBACK MainPanelProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_COMMAND: return SendMessageW(GetParent(h),WM_COMMAND,wp,lp);
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(dc,&rc,gBrBg);
        int W=rc.right;
        int y=6;
        // ── ドロップゾーン (固定52px) ──────────────────────
        RECT dz={8,y,W-8,y+52};
        HBRUSH dzBr=CreateSolidBrush(C_BG2); FillRect(dc,&dz,dzBr); DeleteObject(dzBr);
        DrawRoundRect(dc,dz,8,C_BORDER2);
        std::wstring inText=CleanPath(GetT(gIn));
        SetBkMode(dc,TRANSPARENT);
        HFONT sf=(HFONT)SelectObject(dc,gFontSmall);
        if(inText.empty()){
            SetTextColor(dc,C_TEXT3);
            HFONT of=(HFONT)SelectObject(dc,gFontUI);
            RECT dt=dz; dt.left+=14; dt.top+=4;
            DrawTextW(dc,L"ファイル / フォルダをドロップ",-1,&dt,DT_LEFT|DT_TOP|DT_SINGLELINE);
            SelectObject(dc,of);
        } else {
            SetTextColor(dc,C_ACCENT);
            HFONT of=(HFONT)SelectObject(dc,gFontBold);
            RECT dt=dz; dt.left+=14; dt.right-=14; dt.top+=6;
            DrawTextW(dc,inText.c_str(),-1,&dt,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_END_ELLIPSIS);
            SelectObject(dc,of);
        }
        y+=58;
        // ── PIPELINE (固定78px) ────────────────────────────
        SetTextColor(dc,C_TEXT3);
        HFONT of2=(HFONT)SelectObject(dc,sf);
        TextOutW(dc,10,y,L"PIPELINE",8); y+=13;
        RECT pipeArea={8,y,W-8,y+60};
        DrawPipeline(dc,pipeArea); y+=66;
        // ── METRICS (固定64px) ─────────────────────────────
        TextOutW(dc,10,y,L"METRICS",7); y+=12;
        RECT metArea={8,y,W-8,y+50};
        DrawMetrics(dc,metArea);
        SelectObject(dc,sf);
        EndPaint(h,&ps); return 0;
    }
    case WM_ERASEBKGND:{ HDC dc=(HDC)wp; RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,gBrBg); return 1; }
    }
    return DefWindowProcW(h,msg,wp,lp);
}

// ── サイドバー WndProc ────────────────────────────────────────
static LRESULT CALLBACK SidebarProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(dc,&rc,gBrBg1);
        // 右ボーダー
        HPEN pen=CreatePen(PS_SOLID,1,C_BORDER); HPEN op=(HPEN)SelectObject(dc,pen);
        MoveToEx(dc,rc.right-1,0,nullptr); LineTo(dc,rc.right-1,rc.bottom);
        SelectObject(dc,op); DeleteObject(pen);
        // "LAYER STACK" ラベル
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT3);
        HFONT of=(HFONT)SelectObject(dc,gFontSmall);
        TextOutW(dc,12,8,L"LAYER STACK",11);
        SelectObject(dc,of);
        EndPaint(h,&ps); return 0;
    }
    case WM_ERASEBKGND:{ HDC dc=(HDC)wp; RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,gBrBg1); return 1; }
    }
    return DefWindowProcW(h,msg,wp,lp);
}

// ── 右パネル WndProc ──────────────────────────────────────────
static LRESULT CALLBACK RightPanelProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_COMMAND: return SendMessageW(GetParent(h),WM_COMMAND,wp,lp);
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(dc,&rc,gBrBg1);
        // 左ボーダー
        HPEN pen=CreatePen(PS_SOLID,1,C_BORDER); HPEN op=(HPEN)SelectObject(dc,pen);
        MoveToEx(dc,0,0,nullptr); LineTo(dc,0,rc.bottom);
        SelectObject(dc,op); DeleteObject(pen);
        // セクションタイトルを描く
        auto drawSec=[&](int y,const wchar_t* t){
            SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_TEXT3);
            HFONT of=(HFONT)SelectObject(dc,gFontSmall);
            TextOutW(dc,10,y,t,(int)wcslen(t));
            HPEN p2=CreatePen(PS_SOLID,1,C_BORDER); HPEN op2=(HPEN)SelectObject(dc,p2);
            MoveToEx(dc,10,y+14,nullptr); LineTo(dc,rc.right-10,y+14);
            SelectObject(dc,op2); DeleteObject(p2);
            SelectObject(dc,of);
        };
        drawSec(4,L"PROFILE");
        drawSec(150,L"COMPRESSION");
        drawSec(258,L"OUTPUT");
        drawSec(312,L"L4 DIMENSION");
        EndPaint(h,&ps); return 0;
    }
    case WM_ERASEBKGND:{ HDC dc=(HDC)wp; RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,gBrBg1); return 1; }
    }
    return DefWindowProcW(h,msg,wp,lp);
}

// ── WM_CTLCOLOR* ──────────────────────────────────────────────
static HBRUSH HandleCtlColor(HDC dc,HWND ctl,int type){
    SetBkColor(dc,C_BG); SetTextColor(dc,C_TEXT);
    return gBrBg;
}

// ── メインWndProc ─────────────────────────────────────────────
static LRESULT CALLBACK HeaderProc(HWND,UINT,WPARAM,LPARAM);

static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        // フォント
        gFontUI   =CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        gFontBold  =CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        gFontMono  =CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        gFontSmall =CreateFontW(-10,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        // ブラシ
        gBrBg    =CreateSolidBrush(C_BG);
        gBrBg1   =CreateSolidBrush(C_BG1);
        gBrBg2   =CreateSolidBrush(C_BG2);
        gBrBg3   =CreateSolidBrush(C_BG3);
        gBrAccent=CreateSolidBrush(C_ACCENT);
        // ヘッダークラス登録
        { WNDCLASSEXW hc={sizeof(hc)};
          hc.lpfnWndProc=HeaderProc; hc.hInstance=GetModuleHandleW(nullptr);
          hc.hbrBackground=nullptr; hc.lpszClassName=L"CPBHeader";
          hc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
          RegisterClassExW(&hc); }
        // トグルクラス登録
        static bool togReg=false;
        if(!togReg){
            WNDCLASSEXW tc={sizeof(tc)};
            tc.lpfnWndProc=ToggleProc; tc.hInstance=GetModuleHandleW(nullptr);
            tc.hbrBackground=nullptr; tc.lpszClassName=L"CPBToggle";
            tc.hCursor=LoadCursorW(nullptr,IDC_HAND);
            RegisterClassExW(&tc); togReg=true;
        }
        auto mk=[&](HWND par,const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr)->HWND{
            HWND r=CreateWindowW(cls,t,WS_CHILD|WS_VISIBLE|st,x,y,w,h,par,(HMENU)(intptr_t)id,nullptr,nullptr);
            SendMessageW(r,WM_SETFONT,(WPARAM)(f?f:gFontUI),TRUE); return r; };

        // ── ヘッダー (固定) ──
        int HDR_H=38;
        HWND hdr=CreateWindowW(L"CPBHeader",nullptr,WS_CHILD|WS_VISIBLE,0,0,900,HDR_H,hw,nullptr,nullptr,nullptr);

        int BODY_Y=HDR_H;
        int SB_W=160;    // サイドバー幅
        int RP_W=220;    // 右パネル幅

        // ── サイドバー ──
        {
            WNDCLASSEXW sc={sizeof(sc)};
            sc.lpfnWndProc=SidebarProc; sc.hInstance=GetModuleHandleW(nullptr);
            sc.hbrBackground=nullptr; sc.lpszClassName=L"CPBSidebar"; sc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            RegisterClassExW(&sc);
            gSidebar=CreateWindowW(L"CPBSidebar",nullptr,WS_CHILD|WS_VISIBLE,0,BODY_Y,SB_W,600,hw,nullptr,nullptr,nullptr);
        }
        // トグルボタン (サイドバー内)
        auto mkTog=[&](const wchar_t* num,int y,int id,bool on)->HWND{
            HWND t=CreateWindowW(L"CPBToggle",num,WS_CHILD|WS_VISIBLE,8,y,SB_W-20,28,gSidebar,(HMENU)(intptr_t)id,nullptr,nullptr);
            SetTogOn(t,on); return t;
        };
        // サイドバー: L1→L2→L3→L4→L5→FIDX (番号順・固定)
        gTogL1  =mkTog(L"L1 Protect",    28,ID_TOG_L1,   true);
        gTogL2  =mkTog(L"L2 Compress",   60,ID_TOG_L2,   true);
        gTogL3  =mkTog(L"L3 Genre DSL",  92,ID_TOG_L3,   true);
        gTogL4  =mkTog(L"L4 4D Map",    124,ID_TOG_L4,   true);
        gTogL5  =mkTog(L"L5 GenDict",   156,ID_TOG_L5,   true);
        gTogFidx=mkTog(L"FIDX Search",  188,ID_TOG_FIDX, true);

        // ── メインパネル ──
        {
            WNDCLASSEXW mc={sizeof(mc)};
            mc.lpfnWndProc=MainPanelProc; mc.hInstance=GetModuleHandleW(nullptr);
            mc.hbrBackground=nullptr; mc.lpszClassName=L"CPBMain"; mc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            RegisterClassExW(&mc);
            gMainPanel=CreateWindowW(L"CPBMain",nullptr,WS_CHILD|WS_VISIBLE,SB_W,BODY_Y,500,600,hw,nullptr,nullptr,nullptr);
        }

        // ── 右パネル ──
        {
            WNDCLASSEXW rc2={sizeof(rc2)};
            rc2.lpfnWndProc=RightPanelProc; rc2.hInstance=GetModuleHandleW(nullptr);
            rc2.hbrBackground=nullptr; rc2.lpszClassName=L"CPBRight"; rc2.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            RegisterClassExW(&rc2);
            gRightPanel=CreateWindowW(L"CPBRight",nullptr,WS_CHILD|WS_VISIBLE,900-RP_W,BODY_Y,RP_W,600,hw,nullptr,nullptr,nullptr);
        }

        // ── 右パネル内コントロール ──
        // プロファイルリスト
        gProfList=CreateWindowW(L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
            8,22,RP_W-16,120,gRightPanel,(HMENU)ID_PROF_LIST,nullptr,nullptr);
        SendMessageW(gProfList,WM_SETFONT,(WPARAM)gFontUI,TRUE);
        for(int i=0;i<NPROFS;i++) SendMessageW(gProfList,LB_ADDSTRING,0,(LPARAM)PROFS[i].name);
        SendMessageW(gProfList,LB_SETCURSEL,0,0);

        // L2
        mk(gRightPanel,L"STATIC",L"L2:",SS_LEFT,8,168,24,16,0,gFontSmall);
        gComboL2=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            34,164,RP_W-44,200,gRightPanel,(HMENU)ID_COMBO_L2,nullptr,nullptr);
        SendMessageW(gComboL2,WM_SETFONT,(WPARAM)gFontUI,TRUE);
        for(auto s:{L"AUTO",L"LZMS",L"MSZIP",L"XPRESS_HUFF",L"XPRESS",L"LZ4",L"ZSTD"})
            SendMessageW(gComboL2,CB_ADDSTRING,0,(LPARAM)s);
        SendMessageW(gComboL2,CB_SETCURSEL,0,0);

        // L1強度
        mk(gRightPanel,L"STATIC",L"L1:",SS_LEFT,8,194,24,16,0,gFontSmall);
        gComboRS=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            34,190,RP_W-44,200,gRightPanel,(HMENU)ID_COMBO_RS,nullptr,nullptr);
        SendMessageW(gComboRS,WM_SETFONT,(WPARAM)gFontUI,TRUE);
        for(auto s:{L"STANDARD",L"MAX",L"STEGO",L"LIGHT",L"NONE"})
            SendMessageW(gComboRS,CB_ADDSTRING,0,(LPARAM)s);
        SendMessageW(gComboRS,CB_SETCURSEL,0,0);

        // 出力形式
        mk(gRightPanel,L"STATIC",L"出力:",SS_LEFT,8,272,32,16,0,gFontSmall);
        gComboFmt=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            44,268,RP_W-54,200,gRightPanel,(HMENU)ID_COMBO_FMT,nullptr,nullptr);
        SendMessageW(gComboFmt,WM_SETFONT,(WPARAM)gFontUI,TRUE);
        for(auto s:{L"CPB (.cpb)",L"ZIP (.zip)",L"MP4 (.mp4)",L"PDF (.pdf)",L"PNG (.png)",L"RAW (.raw)"})
            SendMessageW(gComboFmt,CB_ADDSTRING,0,(LPARAM)s);
        SendMessageW(gComboFmt,CB_SETCURSEL,0,0);

        // シード
        mk(gRightPanel,L"STATIC",L"Seed:",SS_LEFT,8,298,32,16,0,gFontSmall);
        gEditSeed=mk(gRightPanel,L"EDIT",L"DEADBEEFCAFEBABE",WS_BORDER|ES_AUTOHSCROLL,44,294,RP_W-54,22,ID_EDIT_SEED,gFontMono);

        // L4次元数
        mk(gRightPanel,L"STATIC",L"L4:",SS_LEFT,8,322,22,16,0,gFontSmall);
        gComboDim=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            34,318,RP_W-44,200,gRightPanel,(HMENU)ID_COMBO_DIM,nullptr,nullptr);
        SendMessageW(gComboDim,WM_SETFONT,(WPARAM)gFontUI,TRUE);
        for(auto sd:{L"4D",L"8D",L"12D",L"16D"})
            SendMessageW(gComboDim,CB_ADDSTRING,0,(LPARAM)sd);
        SendMessageW(gComboDim,CB_SETCURSEL,0,0);
        // チェックボックス
        gChkLearn  =mk(gRightPanel,L"BUTTON",L"学習モード",BS_AUTOCHECKBOX,8,348,100,20,ID_CHK_LEARN);
        gChkPerFile=mk(gRightPanel,L"BUTTON",L"個別圧縮",  BS_AUTOCHECKBOX,8,370,100,20,ID_CHK_PERFILE);

        // ── メインパネル内コントロール (gMainPanelの子) ──
        // パイプライン+メトリクス描画領域の下から開始
        // MainPanelProc の描画: dropzone(68)+pipe_label(14)+pipeline(68)+met_label(14)+metrics(64) = 228
        int PY=236; // メインパネル座標系でのY開始位置
        // mk2: gMainPanelを親にするショートカット
        auto mk2=[&](const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr)->HWND{
            HWND r=CreateWindowW(cls,t,WS_CHILD|WS_VISIBLE|st,x,y,w,h,gMainPanel,(HMENU)(intptr_t)id,nullptr,nullptr);
            SendMessageW(r,WM_SETFONT,(WPARAM)(f?f:gFontUI),TRUE); return r; };

        mk2(L"STATIC",L"入力:",SS_LEFT,8,PY+3,32,16,0,gFontBold);
        gIn=mk2(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,44,PY,200,24,ID_IN);
        mk2(L"BUTTON",L"ファイル",0,0,PY,60,24,ID_BRIN);
        mk2(L"BUTTON",L"Dir",    0,0,PY,36,24,ID_BRDIR); PY+=28;
        mk2(L"STATIC",L"出力:",SS_LEFT,8,PY+3,32,16,0,gFontBold);
        gOut=mk2(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,44,PY,200,24,ID_OUT);
        mk2(L"BUTTON",L"参照",0,0,PY,42,24,ID_BROUT); PY+=28;
        mk2(L"STATIC",L"L5:",SS_LEFT,8,PY+3,22,16,0,gFontBold);
        gDictL5=mk2(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,34,PY,200,24,ID_DICT_L5);
        mk2(L"BUTTON",L"参照",0,0,PY,42,24,ID_BRL5); PY+=28;
        mk2(L"STATIC",L"L3:",SS_LEFT,8,PY+3,22,16,0,gFontBold);
        gDictL3=mk2(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,34,PY,200,24,ID_DICT_L3);
        mk2(L"BUTTON",L"参照",0,0,PY,42,24,ID_BRL3); PY+=32;
        gBtnPack  =mk2(L"BUTTON",L"▶  PACK",  BS_PUSHBUTTON,8,  PY,140,32,ID_BTN_PACK,  gFontBold);
        gBtnUnpack=mk2(L"BUTTON",L"◀  UNPACK",BS_PUSHBUTTON,154,PY,140,32,ID_BTN_UNPACK,gFontBold);
        mk2(L"BUTTON",L"消去",BS_PUSHBUTTON,0,PY+6,42,20,ID_CLRLOG); PY+=38;
        gProg=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE,8,PY,0,10,gMainPanel,(HMENU)ID_PROG,nullptr,nullptr);
        SendMessageW(gProg,PBM_SETRANGE,0,MAKELPARAM(0,100)); PY+=14;
        gLog=CreateWindowW(L"EDIT",nullptr,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
            8,PY,0,0,gMainPanel,(HMENU)ID_LOG,nullptr,nullptr);
        SendMessageW(gLog,WM_SETFONT,(WPARAM)gFontMono,TRUE);
        // 初期サイズはWM_SIZEで設定される

        DragAcceptFiles(hw,TRUE);

        AppendLog(L"CPB — Cocoa Powder Bottle");
        AppendLog(L"ファイル/フォルダをドロップしてPACKボタンを押してください");
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)HandleCtlColor((HDC)wp,(HWND)lp,msg);
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id==ID_BRIN){ auto p=BrowseOpen(); if(!p.empty()) SetInput(p); return 0; }
        if(id==ID_BRDIR){ auto p=BrowseDir(); if(!p.empty()) SetInput(p); return 0; }
        if(id==ID_BROUT){ auto p=BrowseSave(L"cpb"); if(!p.empty()) SetWindowTextW(gOut,p.c_str()); return 0; }
        if(id==ID_BRL5){ auto p=BrowseDictFile(L"L5辞書\0*.dict\0全てのファイル\0*.*\0\0",L"dict"); if(!p.empty()) SetWindowTextW(gDictL5,p.c_str()); return 0; }
        if(id==ID_BRL3){ auto p=BrowseDictFile(L"L3辞書\0*.cpbdict\0全てのファイル\0*.*\0\0",L"cpbdict"); if(!p.empty()) SetWindowTextW(gDictL3,p.c_str()); return 0; }
        if(id==ID_CLRLOG){ SetWindowTextW(gLog,L""); return 0; }
        if(id==ID_CHK_LEARN&&Chk(gChkLearn)) SetChk(gChkPerFile,true);
        if((id>=ID_TOG_L1&&id<=ID_TOG_FIDX)||id==ID_TOG_L5){
            SendMessageW(gProfList,LB_SETCURSEL,NPROFS-1,0);
            InvalidateRect(gMainPanel,nullptr,FALSE); // パイプライン再描画
        }
        if(id==ID_PROF_LIST&&HIWORD(wp)==LBN_SELCHANGE){
            int sel=(int)SendMessageW(gProfList,LB_GETCURSEL,0,0);
            ApplyProf(sel); // トグルをプロファイルに合わせる
            // サイドバー全トグルを再描画
            for(auto t:{gTogL1,gTogL2,gTogL3,gTogL4,gTogL5,gTogFidx})
                if(t) InvalidateRect(t,nullptr,TRUE);
            InvalidateRect(gMainPanel,nullptr,FALSE); // パイプライン再描画
        }
        if(id==ID_BTN_PACK){
            if(gBusy) return 0;
            auto inW=CleanPath(GetT(gIn)); auto outW=CleanPath(GetT(gOut));
            if(inW.empty()){ MessageBoxW(hw,L"入力を指定してください",L"CPB",MB_OK|MB_ICONWARNING); return 0; }
            if(outW.empty()){
                outW=IsDir(inW)?(inW+L".cpb"):([&]{auto d=inW.rfind(L'.');return(d!=std::wstring::npos?inW.substr(0,d):inW)+L".cpb";}());
                SetWindowTextW(gOut,outW.c_str());
            }
            if(Chk(gChkLearn)&&CleanPath(GetT(gDictL5)).empty()){
                std::wstring ad=outW; auto dd=ad.rfind(L'.'); if(dd!=std::wstring::npos) ad=ad.substr(0,dd); ad+=L".dict";
                SetWindowTextW(gDictL5,ad.c_str());
            }
            // 拡張子を出力形式に合わせる
            CarrierFormat fmt=GetFmt();
            { std::wstring ow=CleanPath(GetT(gOut));
              auto d=ow.rfind(L'.'); if(d!=std::wstring::npos) ow=ow.substr(0,d);
              ow+=carrier_ext(fmt); SetWindowTextW(gOut,ow.c_str()); outW=ow; }
            uint64_t seed=0xDEADBEEFCAFEBABE;
            { wchar_t sb[32]; GetWindowTextW(gEditSeed,sb,32); seed=wcstoull(sb,nullptr,16); }
            int pi=(int)SendMessageW(gProfList,LB_GETCURSEL,0,0);
            wchar_t pn[32]={}; SendMessageW(gProfList,LB_GETTEXT,pi,(LPARAM)pn);
            PackArgs a; a.inW=inW; a.outW=outW;
            a.dictL5W=CleanPath(GetT(gDictL5)); a.dictL3W=CleanPath(GetT(gDictL3));
            a.learning=Chk(gChkLearn); a.perFile=Chk(gChkPerFile);
            a.pl=BuildPL(); a.seed=seed; a.cm=GetL2(); a.rs=GetRS(); a.carrier=fmt;
            a.profName=ws2s(pn);
            a.layerFlags[0]=IsTogOn(gTogL1); a.layerFlags[1]=IsTogOn(gTogL2);
            a.layerFlags[2]=IsTogOn(gTogL3); a.layerFlags[3]=IsTogOn(gTogL4);
            a.layerFlags[4]=IsTogOn(gTogL5); a.layerFlags[5]=IsTogOn(gTogFidx);
            gBusy=true; gPipeStep=0; InvalidateRect(gMainPanel,nullptr,FALSE);
            EnableWindow(gBtnPack,FALSE); EnableWindow(gBtnUnpack,FALSE);
            SetWindowTextW(hw,L"CPB — PACK中...");
            std::thread(RunPack,a).detach();
            return 0;
        }
        if(id==ID_BTN_UNPACK){
            if(gBusy) return 0;
            auto inW=CleanPath(GetT(gIn)); auto outW=CleanPath(GetT(gOut));
            if(inW.empty()){ MessageBoxW(hw,L"入力ファイルを指定してください",L"CPB",MB_OK|MB_ICONWARNING); return 0; }
            if(outW.empty()){
                auto d=inW.rfind(L'.'); outW=(d!=std::wstring::npos?inW.substr(0,d):inW)+L"_out";
                SetWindowTextW(gOut,outW.c_str());
            }
            uint64_t seed=0xDEADBEEFCAFEBABE;
            { wchar_t sb[32]; GetWindowTextW(gEditSeed,sb,32); seed=wcstoull(sb,nullptr,16); }
            UnpackArgs a; a.inW=inW; a.outW=outW;
            a.dictL5W=CleanPath(GetT(gDictL5)); a.dictL3W=CleanPath(GetT(gDictL3));
            a.learning=Chk(gChkLearn); a.pl=BuildPL(); a.seed=seed;
            a.dims=GetDimConfig();
            gBusy=true; gPipeStep=0; InvalidateRect(gMainPanel,nullptr,FALSE);
            EnableWindow(gBtnPack,FALSE); EnableWindow(gBtnUnpack,FALSE);
            SetWindowTextW(hw,L"CPB — UNPACK中...");
            std::thread(RunUnpack,a).detach();
            return 0;
        }
        return 0;
    }
    case WM_DROPFILES:{ HDROP hd=(HDROP)wp; wchar_t buf[MAX_PATH]; DragQueryFileW(hd,0,buf,MAX_PATH); DragFinish(hd); SetInput(buf); InvalidateRect(gMainPanel,nullptr,FALSE); return 0; }
    case WM_LOG:{ auto* t=(std::wstring*)lp; AppendLog(*t); delete t; return 0; }
    case WM_PROG:{ SendMessageW(gProg,PBM_SETPOS,(int)wp,0); gPipeStep=((int)wp*7)/100; InvalidateRect(gMainPanel,nullptr,FALSE); return 0; }
    case WM_DONE:{
        auto* raw=(std::wstring*)lp; std::wstring s=*raw; delete raw;
        bool ok=s.size()>=3&&s[0]==L'O'&&s[1]==L'K';
        std::wstring body=s.substr(3);
        auto sep=body.rfind(L'|');
        std::wstring msg=sep!=std::wstring::npos?body.substr(0,sep):body;
        std::wstring path=sep!=std::wstring::npos?body.substr(sep+1):L"";
        EnableWindow(gBtnPack,TRUE); EnableWindow(gBtnUnpack,TRUE);
        gBusy=false; gPipeStep=ok?7:0;
        InvalidateRect(gMainPanel,nullptr,FALSE);
        if(!ok){
            AppendLog(L"[エラー] "+msg);
            MessageBoxW(hw,msg.c_str(),L"CPB エラー",MB_OK|MB_ICONERROR);
            SetWindowTextW(hw,L"CPB — エラー");
        } else {
            SetWindowTextW(hw,(L"CPB — "+msg).c_str());
            if(!path.empty()&&MessageBoxW(hw,(msg+L"\n\n"+path+L"\n\nエクスプローラーで開きますか？").c_str(),
                L"CPB 完了",MB_YESNO|MB_ICONINFORMATION)==IDYES){
                if(IsDir(path)) ShellExecuteW(nullptr,L"open",L"explorer.exe",path.c_str(),nullptr,SW_SHOW);
                else { std::wstring cmd=L"/select,\""+path+L"\""; ShellExecuteW(nullptr,L"open",L"explorer.exe",cmd.c_str(),nullptr,SW_SHOW); }
            }
        }
        return 0;
    }
    case WM_SIZE:{
        RECT rc; GetClientRect(hw,&rc);
        int W=rc.right, H=rc.bottom;
        static const int SB_W=160, RP_W=220, HDR_H=38;
        int MP_W=W-SB_W-RP_W;
        int MP_H=H-HDR_H;
        // パネルリサイズ
        if(gSidebar)    SetWindowPos(gSidebar,   nullptr,0,      HDR_H,SB_W,MP_H,SWP_NOZORDER);
        if(gMainPanel)  SetWindowPos(gMainPanel,  nullptr,SB_W,  HDR_H,MP_W,MP_H,SWP_NOZORDER);
        if(gRightPanel) SetWindowPos(gRightPanel, nullptr,W-RP_W,HDR_H,RP_W,MP_H,SWP_NOZORDER);
        // ── gMainPanel内コントロール ──────────────────────────────
        // 描画エリア(dropzone+pipeline+metrics)= 229px 固定
        static const int DRAW_H=229, G=4;
        int R=MP_W-8;
        int yIn =DRAW_H,   yOut=yIn+28,  yL5=yOut+28;
        int yL3 =yL5+28,   yBtn=yL3+32,  yPrg=yBtn+40, yLog=yPrg+14;
        int xDir =R-36, xFile=xDir-G-52, xRef=R-44;
        HWND brin =(HWND)GetDlgItem(gMainPanel,ID_BRIN);
        HWND brdir=(HWND)GetDlgItem(gMainPanel,ID_BRDIR);
        HWND brout=(HWND)GetDlgItem(gMainPanel,ID_BROUT);
        HWND brl5 =(HWND)GetDlgItem(gMainPanel,ID_BRL5);
        HWND brl3 =(HWND)GetDlgItem(gMainPanel,ID_BRL3);
        HWND clrl =(HWND)GetDlgItem(gMainPanel,ID_CLRLOG);
        if(gIn)     SetWindowPos(gIn,    nullptr,44,   yIn, xFile-44-G,24,SWP_NOZORDER);
        if(brin)    SetWindowPos(brin,   nullptr,xFile,yIn, 52,24,SWP_NOZORDER);
        if(brdir)   SetWindowPos(brdir,  nullptr,xDir, yIn, 36,24,SWP_NOZORDER);
        if(gOut)    SetWindowPos(gOut,   nullptr,44,   yOut,xRef-44-G,24,SWP_NOZORDER);
        if(brout)   SetWindowPos(brout,  nullptr,xRef, yOut,44,24,SWP_NOZORDER);
        if(gDictL5) SetWindowPos(gDictL5,nullptr,34,   yL5, xRef-34-G,24,SWP_NOZORDER);
        if(brl5)    SetWindowPos(brl5,   nullptr,xRef, yL5, 44,24,SWP_NOZORDER);
        if(gDictL3) SetWindowPos(gDictL3,nullptr,34,   yL3, xRef-34-G,24,SWP_NOZORDER);
        if(brl3)    SetWindowPos(brl3,   nullptr,xRef, yL3, 44,24,SWP_NOZORDER);
        if(gBtnPack)  SetWindowPos(gBtnPack,  nullptr,8,  yBtn,140,32,SWP_NOZORDER);
        if(gBtnUnpack)SetWindowPos(gBtnUnpack,nullptr,154,yBtn,140,32,SWP_NOZORDER);
        if(clrl)    SetWindowPos(clrl,   nullptr,xRef, yBtn+6,44,20,SWP_NOZORDER);
        if(gProg)   SetWindowPos(gProg,  nullptr,8,yPrg,MP_W-16,10,SWP_NOZORDER);
        if(gLog)    SetWindowPos(gLog,   nullptr,8,yLog,MP_W-16,
                        std::max(60,MP_H-yLog-8),SWP_NOZORDER);
        // ── サイドバーのトグル ────────────────────────────────────
        if(gSidebar){
            HWND togs[]={gTogL1,gTogL2,gTogL3,gTogL4,gTogL5,gTogFidx};
            int togY=28, togH=(MP_H-togY-8)/6-2;
            togH=std::max(24,std::min(togH,36));
            for(auto tog:togs){
                if(tog) SetWindowPos(tog,nullptr,6,togY,SB_W-14,togH,SWP_NOZORDER);
                togY+=togH+2;
            }
        }
        InvalidateRect(gSidebar,nullptr,FALSE);
        InvalidateRect(gMainPanel,nullptr,FALSE);
        InvalidateRect(gRightPanel,nullptr,FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO:{ auto* m=(MINMAXINFO*)lp; m->ptMinTrackSize={820,620}; return 0; }
    case WM_DESTROY:
        DeleteObject(gFontUI); DeleteObject(gFontBold); DeleteObject(gFontMono); DeleteObject(gFontSmall);
        DeleteObject(gBrBg); DeleteObject(gBrBg1); DeleteObject(gBrBg2); DeleteObject(gBrBg3); DeleteObject(gBrAccent);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}


// ── ヘッダーWndProc ───────────────────────────────────────────
static LRESULT CALLBACK HeaderProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COMMAND) return SendMessageW(GetParent(h),WM_COMMAND,w,l);
    if(m==WM_PAINT){
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        HBRUSH b=CreateSolidBrush(C_BG1); FillRect(dc,&rc,b); DeleteObject(b);
        HPEN p=CreatePen(PS_SOLID,1,C_BORDER); HPEN op=(HPEN)SelectObject(dc,p);
        MoveToEx(dc,0,rc.bottom-1,nullptr); LineTo(dc,rc.right,rc.bottom-1);
        SelectObject(dc,op); DeleteObject(p);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,C_ACCENT);
        HFONT f=CreateFontW(-18,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        HFONT of=(HFONT)SelectObject(dc,f);
        TextOutW(dc,14,8,L"CPB",3); SelectObject(dc,of); DeleteObject(f);
        SetTextColor(dc,C_TEXT3);
        HFONT fs=CreateFontW(-10,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        of=(HFONT)SelectObject(dc,fs);
        TextOutW(dc,48,13,L"Cocoa Powder Bottle",19); SelectObject(dc,of); DeleteObject(fs);
        EndPaint(h,&ps); return 0;
    }
    if(m==WM_ERASEBKGND){
        HBRUSH b=CreateSolidBrush(C_BG1); RECT r; GetClientRect(h,&r);
        FillRect((HDC)w,&r,b); DeleteObject(b); return 1;
    }
    return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int ns){
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc={sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hi;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=L"CPB"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);
    gWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"CPB",L"CPB — Cocoa Powder Bottle",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,620,
        nullptr,nullptr,hi,nullptr);
    ShowWindow(gWnd,ns); UpdateWindow(gWnd);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return (int)msg.wParam;
}
