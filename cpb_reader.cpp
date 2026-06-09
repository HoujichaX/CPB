// cpb_reader.cpp — CPB Reader (参照・検索アプリ)
// cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO
//    cpb_reader.cpp /Fe:cpb_reader.exe /link /SUBSYSTEM:WINDOWS
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
#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;

#define CPB_NO_ZSTD
#define NO_FRAME_IO
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

// ── カラーパレット ────────────────────────────────────────────
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
#define C_MATCH    RGB(255,230,150)  // 検索ヒット背景

// ── IDs ──────────────────────────────────────────────────────
#define ID_FILELIST   101
#define ID_PREVIEW    102
#define ID_SEARCH     103
#define ID_STATUS     104
#define ID_BTN_OPEN   201
#define ID_BTN_SEARCH 202
#define ID_BTN_EXPORT 203
#define ID_BTN_DICT   204
#define ID_BTN_EXPORTALL 205
#define WM_LOADED  (WM_USER+1)
#define WM_STATUS  (WM_USER+2)

// ── 内部構造 ──────────────────────────────────────────────────
static const uint8_t MULTI_PER[8]={'C','P','B','M','P','E','R','!'};
static const uint8_t META_MAGIC[8]={'C','P','B','M','E','T','A','!'};
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

struct FileEntry {
    std::string  rel;       // 相対パス (UTF-8)
    std::wstring relW;      // ワイド
    std::vector<uint8_t> comp; // 圧縮済みデータ
    size_t origSize;         // 元サイズ
    bool decoded=false;
    std::vector<uint8_t> data; // デコード済み (遅延展開)
    bool isBinary=false;
};

struct Archive {
    std::wstring path;
    std::vector<FileEntry> files;
    LayerPipeline pl;
    CPBConfig cfg;
    std::string metaProfile;
    std::string neededDict;
    bool hasL5Dict=false;
};

// ── グローバル ────────────────────────────────────────────────
static HWND gWnd,gFileList,gPreview,gSearch,gStatus;
static HWND gBtnOpen,gBtnSearch,gBtnExport,gBtnDict,gBtnExportAll;
static HWND gLeftPanel,gRightPanel,gHeader;
static HFONT gFontUI,gFontBold,gFontMono,gFontSmall;
static HBRUSH gBrBg,gBrBg1,gBrBg2;
static Archive gArc;
static int gSelected=-1;
static std::wstring gDictPath;
static std::atomic<bool> gLoading{false};

// ── ユーティリティ ─────────────────────────────────────────────
static std::wstring GetT(HWND h){
    int n=GetWindowTextLengthW(h); if(n<=0) return L"";
    std::wstring s(n+1,0); GetWindowTextW(h,&s[0],n+1); s.resize(n); return s;
}
static std::wstring CleanPath(std::wstring p){
    while(!p.empty()&&(p.front()==L'"'||p.front()==L' ')) p.erase(p.begin());
    while(!p.empty()&&(p.back()==L'"'||p.back()==L' '))  p.pop_back();
    return p;
}
static bool ReadFile2(const std::wstring& p,std::vector<uint8_t>& out){
    FILE* f=_wfopen(p.c_str(),L"rb"); if(!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    out.resize(sz); if(sz>0) fread(out.data(),1,sz,f);
    fclose(f); return true;
}
static bool WriteFile2(const std::wstring& p,const void* d,size_t sz){
    FILE* f=_wfopen(p.c_str(),L"wb"); if(!f) return false;
    fwrite(d,1,sz,f); fclose(f); return true;
}
static std::string ws2s(const std::wstring& w){
    if(w.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string r(n-1,0); WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,&r[0],n,nullptr,nullptr);
    return r;
}
static std::wstring s2ws(const std::string& s){
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring r(n-1,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&r[0],n);
    return r;
}
static bool IsBinary(const std::vector<uint8_t>& d){
    size_t check=std::min(d.size(),(size_t)512);
    int nulls=0;
    for(size_t i=0;i<check;i++) if(d[i]==0) nulls++;
    return nulls > (int)(check/10);
}
static void SetStatus(const std::wstring& t){
    PostMessageW(gWnd,WM_STATUS,0,(LPARAM)new std::wstring(t));
}

// ── メタデータ読み取り ─────────────────────────────────────────
static std::string JsonGet(const std::string& j,const std::string& key){
    auto p=j.find("\""+key+"\":");
    if(p==std::string::npos) return "";
    p+=key.size()+3;
    if(j[p]=='"'){++p;std::string v;while(p<j.size()&&j[p]!='"')v+=j[p++];return v;}
    std::string v;while(p<j.size()&&j[p]!=','&&j[p]!='}')v+=j[p++];return v;
}
static void ReadMeta(std::vector<uint8_t>& c,Archive& arc){
    if(c.size()<12) return;
    size_t end=c.size();
    if(memcmp(c.data()+end-8,META_MAGIC,8)!=0) return;
    uint32_t jsz=0; for(int i=0;i<4;i++) jsz|=((uint32_t)c[end-12+i])<<(i*8);
    if(jsz+12>c.size()) return;
    size_t js=end-12-jsz;
    std::string json(c.begin()+js,c.begin()+js+jsz);
    c.resize(js);
    arc.metaProfile=JsonGet(json,"profile");
    arc.neededDict=JsonGet(json,"dict");
    bool learning=(JsonGet(json,"learn")=="true");
    std::string lstr=JsonGet(json,"layers");
    std::vector<LayerID> ord;
    if(lstr.size()>=6){
        if(lstr[4]=='1') ord.push_back(learning?LayerID::L5_LEARN:LayerID::L5);
        if(lstr[2]=='1') ord.push_back(LayerID::L3);
        if(lstr[1]=='1') ord.push_back(LayerID::L2);
        if(lstr[0]=='1') ord.push_back(LayerID::L1);
        if(lstr[3]=='1') ord.push_back(LayerID::L4B);
        if(lstr[5]=='1') ord.push_back(LayerID::FIDX);
    }
    arc.pl=ord.empty()?LayerPipeline::standard():LayerPipeline::custom(ord);
    arc.cfg.learning=learning;
    std::string ss=JsonGet(json,"seed");
    arc.cfg.fourd_seed=ss.empty()?0xDEADBEEFCAFEBABEULL:strtoull(ss.c_str(),nullptr,16);
    arc.hasL5Dict=learning&&!arc.neededDict.empty();
}

// ── アーカイブ読み込み ─────────────────────────────────────────
static bool LoadArchive(const std::wstring& path){
    std::vector<uint8_t> rawFile;
    if(!ReadFile2(path,rawFile)) return false;
    // キャリア剥がし
    std::vector<uint8_t> container;
    if(is_cpb_carrier(rawFile)) container=carrier_unwrap(rawFile);
    else container=rawFile;
    // メタデータ
    Archive arc; arc.path=path;
    ReadMeta(container,arc);
    // コンテナ解析
    auto be=make_backend(BackendType::RAW);
    if(!be->deserialize(container)) return false;
    auto vf=be->read_frame(0);
    // フォーマット判定
    if(vf.data.size()<8) return false;
    if(memcmp(vf.data.data(),MULTI_PER,8)==0){
        // 個別圧縮形式
        const uint8_t* p=vf.data.data()+8;
        uint32_t n=0; for(int i=0;i<4;i++) n|=((uint32_t)p[i])<<(i*8); p+=4;
        for(uint32_t i=0;i<n;i++){
            if(p+4>vf.data.data()+vf.data.size()) break;
            uint32_t plen=0; for(int j=0;j<4;j++) plen|=((uint32_t)p[j])<<(j*8); p+=4;
            std::string rel((const char*)p,plen); p+=plen;
            uint64_t osz=0; for(int j=0;j<8;j++) osz|=((uint64_t)p[j])<<(j*8); p+=8;
            uint64_t csz=0; for(int j=0;j<8;j++) csz|=((uint64_t)p[j])<<(j*8); p+=8;
            if(p+csz>vf.data.data()+vf.data.size()) break;
            FileEntry fe;
            fe.rel=rel; fe.relW=s2ws(rel);
            fe.comp.assign(p,p+csz); p+=csz;
            fe.origSize=osz;
            arc.files.push_back(std::move(fe));
        }
    } else {
        // 単体ファイル or 全体圧縮
        static const uint8_t MHDR[8]={0x43,0x50,0x42,0x4D,0x55,0x4C,0x54,0x49};
        if(memcmp(vf.data.data(),MHDR,8)==0){
            // 全体圧縮フォルダ
            const uint8_t* p=vf.data.data()+8;
            uint32_t n=0; for(int i=0;i<4;i++) n|=((uint32_t)p[i])<<(i*8); p+=4;
            for(uint32_t i=0;i<n;i++){
                if(p+4>vf.data.data()+vf.data.size()) break;
                uint32_t plen=0; for(int j=0;j<4;j++) plen|=((uint32_t)p[j])<<(j*8); p+=4;
                std::string rel((const char*)p,plen); p+=plen;
                uint64_t fsz=0; for(int j=0;j<8;j++) fsz|=((uint64_t)p[j])<<(j*8); p+=8;
                if(p+fsz>vf.data.data()+vf.data.size()) break;
                FileEntry fe;
                fe.rel=rel; fe.relW=s2ws(rel);
                fe.data.assign(p,p+fsz); p+=fsz;
                fe.origSize=fsz; fe.decoded=true;
                fe.isBinary=IsBinary(fe.data);
                arc.files.push_back(std::move(fe));
            }
        } else {
            // 単体ファイル
            FileEntry fe;
            auto fn=path.rfind(L'\\');
            std::wstring name=(fn!=std::wstring::npos)?path.substr(fn+1):path;
            fe.relW=name; fe.rel=ws2s(name);
            fe.comp=vf.data; fe.origSize=vf.data.size();
            arc.files.push_back(std::move(fe));
        }
    }
    gArc=std::move(arc);
    gSelected=-1;
    return true;
}

// ── ファイル展開 ──────────────────────────────────────────────
static bool DecodeEntry(FileEntry& fe){
    if(fe.decoded) return true;
    if(fe.comp.empty()) return false;
    // L5辞書ロード
    if(gArc.cfg.learning&&!gDictPath.empty())
        l5_cache_load(gDictPath);
    gArc.cfg.l3_dict_path=ws2s(gDictPath);
    auto dec=run_pipeline_decode(fe.comp,gArc.pl,gArc.cfg);
    if(!dec.success) return false;
    fe.data=dec.data;
    fe.decoded=true;
    fe.isBinary=IsBinary(fe.data);
    return true;
}

// ── プレビュー表示 ────────────────────────────────────────────
static void ShowPreview(int idx){
    if(idx<0||idx>=(int)gArc.files.size()){
        SetWindowTextW(gPreview,L"");
        EnableWindow(gBtnExport,FALSE);
        return;
    }
    auto& fe=gArc.files[idx];
    EnableWindow(gBtnExport,TRUE);
    if(!fe.decoded){
        SetWindowTextW(gPreview,L"展開中...");
        if(!DecodeEntry(fe)){
            std::wstring msg=L"展開できませんでした。\n\n";
            if(gArc.hasL5Dict&&gDictPath.empty())
                msg+=L"このファイルにはL5辞書が必要です。\n辞書: "+s2ws(gArc.neededDict)+L"\n\n「辞書指定」ボタンで辞書を設定してください。";
            else
                msg+=L"プロファイル: "+s2ws(gArc.metaProfile);
            SetWindowTextW(gPreview,msg.c_str());
            return;
        }
    }
    if(fe.isBinary){
        // バイナリ: HEXダンプ
        std::wstring hex;
        size_t show=std::min(fe.data.size(),(size_t)512);
        for(size_t i=0;i<show;i+=16){
            wchar_t line[80]={};
            swprintf(line,80,L"%04zX: ",i);
            hex+=line;
            for(size_t j=0;j<16&&i+j<show;j++){
                wchar_t b[8]; swprintf(b,8,L"%02X ",fe.data[i+j]);
                hex+=b;
            }
            hex+=L" |";
            for(size_t j=0;j<16&&i+j<show;j++){
                uint8_t c=fe.data[i+j];
                hex+=(c>=0x20&&c<0x7F)?(wchar_t)c:L'.';
            }
            hex+=L"|\r\n";
        }
        if(fe.data.size()>512){
            wchar_t b[64]; swprintf(b,64,L"\r\n... (%zuB 省略)",fe.data.size()-512);
            hex+=b;
        }
        SetWindowTextW(gPreview,hex.c_str());
    } else {
        // テキスト: UTF-8としてデコード
        int wsz=MultiByteToWideChar(CP_UTF8,0,(const char*)fe.data.data(),(int)fe.data.size(),nullptr,0);
        std::wstring text(wsz,0);
        MultiByteToWideChar(CP_UTF8,0,(const char*)fe.data.data(),(int)fe.data.size(),&text[0],wsz);
        // \n → \r\n
        std::wstring crlf;
        for(wchar_t c:text){ if(c==L'\n') crlf+=L'\r'; crlf+=c; }
        SetWindowTextW(gPreview,crlf.c_str());
    }
}

// ── 検索 ─────────────────────────────────────────────────────
static void DoSearch(const std::wstring& kw){
    if(kw.empty()||gArc.files.empty()) return;
    std::string kwU=ws2s(kw);
    // 全ファイルを展開して検索
    int hits=0;
    SendMessageW(gFileList,LB_RESETCONTENT,0,0);
    for(int i=0;i<(int)gArc.files.size();i++){
        auto& fe=gArc.files[i];
        if(!fe.decoded) DecodeEntry(fe);
        bool match=false;
        if(!fe.data.empty()&&!fe.isBinary){
            std::string content((const char*)fe.data.data(),fe.data.size());
            match=(content.find(kwU)!=std::string::npos);
        }
        if(match||kw==L"*"){
            std::wstring label=(match?L"● ":L"  ")+fe.relW;
            SendMessageW(gFileList,LB_ADDSTRING,0,(LPARAM)label.c_str());
            SendMessageW(gFileList,LB_SETITEMDATA,
                SendMessageW(gFileList,LB_GETCOUNT,0,0)-1,(LPARAM)i);
            hits++;
        }
    }
    wchar_t b[64]; swprintf(b,64,L"%d件ヒット",hits);
    SetWindowTextW(gStatus,b);
}

// ── ファイルリスト再構築 ──────────────────────────────────────
static void RebuildList(){
    SendMessageW(gFileList,LB_RESETCONTENT,0,0);
    for(int i=0;i<(int)gArc.files.size();i++){
        auto& fe=gArc.files[i];
        // アイコン風プレフィックス
        std::wstring ext;
        auto d=fe.relW.rfind(L'.');
        if(d!=std::wstring::npos) ext=fe.relW.substr(d+1);
        for(auto& c:ext) c=towlower(c);
        std::wstring icon=L"📄 ";
        if(ext==L"json"||ext==L"xml"||ext==L"html") icon=L"{ } ";
        else if(ext==L"csv"||ext==L"tsv") icon=L"⊞  ";
        else if(ext==L"txt"||ext==L"log") icon=L"≡  ";
        else if(ext==L"jpg"||ext==L"png") icon=L"🖼 ";
        SendMessageW(gFileList,LB_ADDSTRING,0,(LPARAM)(icon+fe.relW).c_str());
        SendMessageW(gFileList,LB_SETITEMDATA,i,(LPARAM)i);
    }
    // 件数とサイズをステータスに
    size_t total=0; for(auto& fe:gArc.files) total+=fe.origSize;
    wchar_t b[128];
    auto fn=gArc.path.rfind(L'\\');
    std::wstring name=(fn!=std::wstring::npos)?gArc.path.substr(fn+1):gArc.path;
    swprintf(b,128,L"%s  |  %dファイル  |  %.1fKB",name.c_str(),(int)gArc.files.size(),(double)total/1024.0);
    SetWindowTextW(gStatus,b);
    EnableWindow(gBtnExportAll,!gArc.files.empty());
}

// ── パネルWndProc ─────────────────────────────────────────────
static LRESULT CALLBACK PanelProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COMMAND) return SendMessageW(GetParent(h),WM_COMMAND,w,l);
    if(m==WM_ERASEBKGND){
        HBRUSH b=(h==gRightPanel)?gBrBg:gBrBg1;
        RECT r; GetClientRect(h,&r); FillRect((HDC)w,&r,b); return 1;
    }
    if(m==WM_PAINT){
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(dc,&rc,(h==gRightPanel)?gBrBg:gBrBg1);
        if(h==gRightPanel){
            HPEN p=CreatePen(PS_SOLID,1,C_BORDER); HPEN op=(HPEN)SelectObject(dc,p);
            MoveToEx(dc,0,0,nullptr); LineTo(dc,0,rc.bottom);
            SelectObject(dc,op); DeleteObject(p);
        }
        EndPaint(h,&ps); return 0;
    }
    return DefWindowProcW(h,m,w,l);
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
        TextOutW(dc,14,8,L"CPB Reader",10); SelectObject(dc,of); DeleteObject(f);
        EndPaint(h,&ps); return 0;
    }
    if(m==WM_ERASEBKGND){
        HBRUSH b=CreateSolidBrush(C_BG1); RECT r; GetClientRect(h,&r);
        FillRect((HDC)w,&r,b); DeleteObject(b); return 1;
    }
    return DefWindowProcW(h,m,w,l);
}

// ── WndProc ───────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        gFontUI   =CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        gFontBold  =CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        gFontMono  =CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        gFontSmall =CreateFontW(-10,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        gBrBg =CreateSolidBrush(C_BG);
        gBrBg1=CreateSolidBrush(C_BG1);
        gBrBg2=CreateSolidBrush(C_BG2);

        // クラス登録
        auto regClass=[&](const wchar_t* name,WNDPROC proc){
            WNDCLASSEXW c={sizeof(c)};
            c.lpfnWndProc=proc; c.hInstance=GetModuleHandleW(nullptr);
            c.hbrBackground=nullptr; c.lpszClassName=name;
            c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            RegisterClassExW(&c);
        };
        regClass(L"CPBRPanel",PanelProc);
        regClass(L"CPBRHeader",HeaderProc);

        auto mk=[&](HWND par,const wchar_t* cls,const wchar_t* t,DWORD st,
                    int x,int y,int w,int h,int id,HFONT f=nullptr)->HWND{
            HWND r=CreateWindowW(cls,t,WS_CHILD|WS_VISIBLE|st,x,y,w,h,par,(HMENU)(intptr_t)id,nullptr,nullptr);
            SendMessageW(r,WM_SETFONT,(WPARAM)(f?f:gFontUI),TRUE); return r; };

        int HDR_H=38;
        gHeader=CreateWindowW(L"CPBRHeader",nullptr,WS_CHILD|WS_VISIBLE,0,0,800,HDR_H,hw,nullptr,nullptr,nullptr);
        // ヘッダー内ボタン
        gBtnOpen=mk(gHeader,L"BUTTON",L"📂 開く",BS_PUSHBUTTON,HDR_H*4+4,5,90,28,ID_BTN_OPEN,gFontBold);

        int BY=HDR_H;
        // レイアウト: 左=ファイルリスト、右=プレビュー
        int LP_W=260;
        gLeftPanel =CreateWindowW(L"CPBRPanel",nullptr,WS_CHILD|WS_VISIBLE,0,  BY,LP_W,580,hw,nullptr,nullptr,nullptr);
        gRightPanel=CreateWindowW(L"CPBRPanel",nullptr,WS_CHILD|WS_VISIBLE,LP_W,BY,540,580,hw,nullptr,nullptr,nullptr);

        // 左パネル
        {
            auto lp=[&](const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr){
                return mk(gLeftPanel,cls,t,st,x,y,w,h,id,f); };
            // 検索バー
            lp(L"STATIC",L"🔍",SS_LEFT,6,6,18,22,0);
            gSearch=lp(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,26,6,LP_W-80,24,ID_SEARCH);
            gBtnSearch=lp(L"BUTTON",L"検索",BS_PUSHBUTTON,LP_W-52,6,48,24,ID_BTN_SEARCH);
            // ファイルリスト
            lp(L"STATIC",L"FILES",SS_LEFT,8,36,60,14,0,gFontSmall);
            gFileList=CreateWindowW(L"LISTBOX",nullptr,
                WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
                6,52,LP_W-12,480,gLeftPanel,(HMENU)ID_FILELIST,nullptr,nullptr);
            SendMessageW(gFileList,WM_SETFONT,(WPARAM)gFontMono,TRUE);
            // 辞書指定ボタン
            gBtnDict=lp(L"BUTTON",L"🔑 辞書指定",BS_PUSHBUTTON,6,536,114,24,ID_BTN_DICT);
            // 全て展開
            gBtnExportAll=lp(L"BUTTON",L"📦 全て展開",BS_PUSHBUTTON,122,536,LP_W-128,24,ID_BTN_EXPORTALL);
            EnableWindow(gBtnExportAll,FALSE);
        }

        // 右パネル
        {
            auto rp=[&](const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr){
                return mk(gRightPanel,cls,t,st,x,y,w,h,id,f); };
            rp(L"STATIC",L"PREVIEW",SS_LEFT,8,6,80,14,0,gFontSmall);
            // このファイルを取り出すボタン
            gBtnExport=mk(gRightPanel,L"BUTTON",L"💾 このファイルを取り出す",BS_PUSHBUTTON,0,6,200,24,ID_BTN_EXPORT,gFontBold);
            EnableWindow(gBtnExport,FALSE);
            // プレビュー
            gPreview=CreateWindowW(L"EDIT",nullptr,
                WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|WS_HSCROLL|
                ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
                8,34,0,0,gRightPanel,(HMENU)ID_PREVIEW,nullptr,nullptr);
            SendMessageW(gPreview,WM_SETFONT,(WPARAM)gFontMono,TRUE);
        }

        // ステータスバー
        gStatus=mk(hw,L"STATIC",L"ファイルをドロップするか「開く」ボタンで読み込んでください",
            SS_LEFT|SS_SUNKEN,0,0,800,22,ID_STATUS,gFontSmall);

        DragAcceptFiles(hw,TRUE);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp,C_BG); SetTextColor((HDC)wp,C_TEXT); return (LRESULT)gBrBg;
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id==ID_BTN_OPEN){
            wchar_t buf[MAX_PATH]={};
            OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=hw; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
            o.lpstrFilter=L"CPBファイル\0*.cpb;*.raw;*.zip;*.mp4;*.pdf;*.png\0全てのファイル\0*.*\0\0";
            o.Flags=OFN_FILEMUSTEXIST;
            if(GetOpenFileNameW(&o)){
                SetWindowTextW(hw,L"CPB Reader — 読み込み中...");
                if(LoadArchive(buf)){
                    RebuildList();
                    SetWindowTextW(hw,L"CPB Reader");
                } else {
                    MessageBoxW(hw,L"ファイルを読み込めませんでした",L"エラー",MB_OK|MB_ICONERROR);
                    SetWindowTextW(hw,L"CPB Reader");
                }
            }
            return 0;
        }
        if(id==ID_BTN_SEARCH||(id==ID_SEARCH&&HIWORD(wp)==EN_CHANGE)){
            if(id==ID_BTN_SEARCH||GetKeyState(VK_RETURN)<0){
                std::wstring kw=GetT(gSearch);
                if(!kw.empty()) DoSearch(kw);
                else RebuildList();
            }
            return 0;
        }
        if(id==ID_FILELIST&&HIWORD(wp)==LBN_SELCHANGE){
            int sel=(int)SendMessageW(gFileList,LB_GETCURSEL,0,0);
            if(sel>=0){
                int idx=(int)SendMessageW(gFileList,LB_GETITEMDATA,sel,0);
                gSelected=idx;
                ShowPreview(idx);
            }
            return 0;
        }
        if(id==ID_BTN_EXPORT){
            if(gSelected<0||gSelected>=(int)gArc.files.size()) return 0;
            auto& fe=gArc.files[gSelected];
            if(!fe.decoded&&!DecodeEntry(fe)){
                MessageBoxW(hw,L"ファイルを展開できませんでした",L"エラー",MB_OK|MB_ICONERROR);
                return 0;
            }
            wchar_t buf[MAX_PATH]={};
            // ファイル名を自動設定
            auto p=fe.relW.rfind(L'/'); if(p==std::wstring::npos) p=fe.relW.rfind(L'\\');
            std::wstring fname=(p!=std::wstring::npos)?fe.relW.substr(p+1):fe.relW;
            wcsncpy(buf,fname.c_str(),MAX_PATH-1);
            OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=hw; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
            o.lpstrFilter=L"全てのファイル\0*.*\0\0"; o.Flags=OFN_OVERWRITEPROMPT;
            if(GetSaveFileNameW(&o)){
                if(WriteFile2(buf,fe.data.data(),fe.data.size()))
                    MessageBoxW(hw,(std::wstring(buf)+L"\nに保存しました").c_str(),L"完了",MB_OK|MB_ICONINFORMATION);
                else
                    MessageBoxW(hw,L"保存に失敗しました",L"エラー",MB_OK|MB_ICONERROR);
            }
            return 0;
        }
        if(id==ID_BTN_EXPORTALL){
            // 全ファイルを展開して指定フォルダに保存
            IFileDialog* pfd=nullptr;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_ALL,
                IID_IFileOpenDialog,(void**)&pfd))) return 0;
            DWORD opts; pfd->GetOptions(&opts);
            pfd->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
            pfd->SetTitle(L"展開先フォルダを選択");
            if(SUCCEEDED(pfd->Show(hw))){
                IShellItem* item=nullptr;
                if(SUCCEEDED(pfd->GetResult(&item))){
                    PWSTR outDir=nullptr;
                    if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&outDir))){
                        int ok=0,fail=0;
                        SetWindowTextW(hw,L"CPB Reader — 展開中...");
                        for(auto& fe:gArc.files){
                            if(!fe.decoded) DecodeEntry(fe);
                            if(!fe.decoded){fail++; continue;}
                            // パスを構築
                            std::wstring relW=fe.relW;
                            for(auto& c:relW) if(c==L'/') c=L'\\';
                            fs::path dst=fs::path(outDir)/relW;
                            std::error_code ec;
                            fs::create_directories(dst.parent_path(),ec);
                            WriteFile2(dst.wstring(),fe.data.data(),fe.data.size());
                            ok++;
                        }
                        CoTaskMemFree(outDir);
                        wchar_t b[128]; swprintf(b,128,L"%d個展開 / %d個失敗",ok,fail);
                        MessageBoxW(hw,b,L"展開完了",MB_OK|MB_ICONINFORMATION);
                        SetWindowTextW(hw,L"CPB Reader");
                    }
                    item->Release();
                }
            }
            pfd->Release();
            return 0;
        }
        if(id==ID_BTN_DICT){
            wchar_t buf[MAX_PATH]={};
            OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=hw; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
            o.lpstrFilter=L"L5辞書\0*.dict\0L3辞書\0*.cpbdict\0全てのファイル\0*.*\0\0";
            o.Flags=OFN_FILEMUSTEXIST;
            if(GetOpenFileNameW(&o)){
                gDictPath=buf;
                // 拡張子で種類判定
                std::wstring ext=gDictPath; auto d=ext.rfind(L'.'); if(d!=std::wstring::npos) ext=ext.substr(d+1);
                for(auto& c:ext) c=towlower(c);
                if(ext==L"dict"){
                    l5_cache_load(gDictPath);
                    wchar_t b[64]; swprintf(b,64,L"L5辞書: %zuエントリ ロード済み",l5_learn_cache_size());
                    SetWindowTextW(gStatus,b);
                } else {
                    gArc.cfg.l3_dict_path=ws2s(gDictPath);
                    SetWindowTextW(gStatus,(L"L3辞書: "+gDictPath+L" 設定済み").c_str());
                }
                // 選択中ファイルを再表示
                if(gSelected>=0){
                    gArc.files[gSelected].decoded=false;
                    gArc.files[gSelected].data.clear();
                    ShowPreview(gSelected);
                }
            }
            return 0;
        }
        return 0;
    }
    case WM_DROPFILES:{
        HDROP hd=(HDROP)wp; wchar_t buf[MAX_PATH];
        DragQueryFileW(hd,0,buf,MAX_PATH); DragFinish(hd);
        SetWindowTextW(hw,L"CPB Reader — 読み込み中...");
        if(LoadArchive(buf)){
            RebuildList();
            SetWindowTextW(hw,L"CPB Reader");
        } else {
            MessageBoxW(hw,L"ファイルを読み込めませんでした",L"エラー",MB_OK|MB_ICONERROR);
            SetWindowTextW(hw,L"CPB Reader");
        }
        return 0;
    }
    case WM_STATUS:{ auto* t=(std::wstring*)lp; SetWindowTextW(gStatus,t->c_str()); delete t; return 0; }
    case WM_SIZE:{
        RECT rc; GetClientRect(hw,&rc);
        int w=rc.right,h=rc.bottom;
        int HDR_H=38,ST_H=22,LP_W=260;
        int RP_W=w-LP_W;
        if(gHeader)     SetWindowPos(gHeader,    nullptr,0,    0,    w,   HDR_H,SWP_NOZORDER);
        if(gLeftPanel)  SetWindowPos(gLeftPanel,  nullptr,0,    HDR_H,LP_W,h-HDR_H-ST_H,SWP_NOZORDER);
        if(gRightPanel) SetWindowPos(gRightPanel, nullptr,LP_W, HDR_H,RP_W,h-HDR_H-ST_H,SWP_NOZORDER);
        if(gStatus)     SetWindowPos(gStatus,     nullptr,0,    h-ST_H,w, ST_H,SWP_NOZORDER);
        // 右パネル内コントロール
        int bodyH=h-HDR_H-ST_H;
        if(gBtnExport) SetWindowPos(gBtnExport,nullptr,RP_W-210,6,  204,24,SWP_NOZORDER);
        if(gPreview)   SetWindowPos(gPreview,  nullptr,8,      34,  RP_W-16,bodyH-42,SWP_NOZORDER);
        // 左パネル内
        HWND bsrc=(HWND)GetDlgItem(gLeftPanel,ID_BTN_SEARCH);
        if(gSearch)  SetWindowPos(gSearch, nullptr,26,6,LP_W-82,24,SWP_NOZORDER);
        if(bsrc)     SetWindowPos(bsrc,   nullptr,LP_W-54,6,50,24,SWP_NOZORDER);
        if(gFileList)SetWindowPos(gFileList,nullptr,6,52,LP_W-12,bodyH-88,SWP_NOZORDER);
        if(gBtnDict) SetWindowPos(gBtnDict,nullptr,6,bodyH-32,120,24,SWP_NOZORDER);
        if(gBtnExportAll)SetWindowPos(gBtnExportAll,nullptr,130,bodyH-32,LP_W-136,24,SWP_NOZORDER);
        // ヘッダー内ボタン
        if(gBtnOpen) SetWindowPos(gBtnOpen,nullptr,w-100,5,92,28,SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO:{ auto* m=(MINMAXINFO*)lp; m->ptMinTrackSize={700,500}; return 0; }
    case WM_DESTROY:
        DeleteObject(gFontUI); DeleteObject(gFontBold); DeleteObject(gFontMono); DeleteObject(gFontSmall);
        DeleteObject(gBrBg); DeleteObject(gBrBg1); DeleteObject(gBrBg2);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR cmdline,int ns){
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES};
    InitCommonControlsEx(&ic);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc={sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hi;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=L"CPBReader";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);
    gWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"CPBReader",
        L"CPB Reader",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,800,600,
        nullptr,nullptr,hi,nullptr);
    ShowWindow(gWnd,ns); UpdateWindow(gWnd);
    // コマンドライン引数でファイルを開く
    if(cmdline&&cmdline[0]){
        std::wstring path=CleanPath(std::wstring(cmdline));
        if(!path.empty()&&LoadArchive(path)) RebuildList();
    }
    MSG m;
    while(GetMessageW(&m,nullptr,0,0)){ TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return (int)m.wParam;
}
