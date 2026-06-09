// cpb_train.cpp — CPB 辞書訓練アプリ (Warm Design)
// cl /std:c++17 /O2 /EHsc /nologo /utf-8 cpb_train.cpp /Fe:cpb_train.exe
//    /link /SUBSYSTEM:WINDOWS
//    user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")

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
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
namespace fs = std::filesystem;

// ── カラーパレット (cpb_gui.cpp と統一) ──────────────────────
#define C_BG       RGB(253,246,240)
#define C_BG1      RGB(248,238,226)
#define C_BG2      RGB(241,228,212)
#define C_BORDER   RGB(210,186,160)
#define C_BORDER2  RGB(185,155,120)
#define C_ACCENT   RGB(176,82,22)
#define C_TEXT     RGB(55,38,24)
#define C_TEXT2    RGB(110,85,58)
#define C_TEXT3    RGB(155,128,98)
#define C_OK       RGB(72,128,76)

// ── IDs ──────────────────────────────────────────────────────
#define ID_LIST         101
#define ID_OUT_L3       102
#define ID_OUT_L5       103
#define ID_LOG          104
#define ID_BTN_ADDDIR   201
#define ID_BTN_ADDFILE  202
#define ID_BTN_CLEAR    203
#define ID_BTN_TRAIN    204
#define ID_BTN_BRL3     205
#define ID_BTN_BRL5     206
#define ID_EDIT_PHRASES 301
#define ID_EDIT_MINFREQ 302
#define ID_EDIT_MINLEN  303
#define ID_EDIT_MAXLEN  304
#define ID_PROG         401
#define WM_LOG  (WM_USER+1)
#define WM_PROG (WM_USER+2)
#define WM_DONE (WM_USER+3)

static const uint8_t DICT_MAGIC[8]={'C','P','B','D','I','C','T','!'};
static const uint8_t L5P_MAGIC[8]={0xC5,0xCA,0xC4,0x08,0x50,0x45,0x52,0x53};

// ── グローバル ────────────────────────────────────────────────
static HWND gWnd,gList,gLog,gProg;
static HWND gOutL3,gOutL5;
static HWND gEdPhrases,gEdMinFreq,gEdMinLen,gEdMaxLen;
static HWND gBtnTrain;
static HWND gLeftPanel,gRightPanel;
static HFONT gFontUI,gFontBold,gFontMono,gFontSmall;
static HBRUSH gBrBg,gBrBg1,gBrBg2;
static std::vector<std::wstring> gFiles;
static std::atomic<bool> gBusy{false};

// ── ユーティリティ ─────────────────────────────────────────────
static std::wstring GetT(HWND h){
    int n=GetWindowTextLengthW(h); if(n<=0) return L"";
    std::wstring s(n+1,0); GetWindowTextW(h,&s[0],n+1); s.resize(n); return s;
}
static int GetI(HWND h,int def){
    try{ return std::stoi(GetT(h)); }catch(...){ return def; }
}
static void AppendLog(const std::wstring& t){
    int n=GetWindowTextLengthW(gLog);
    SendMessageW(gLog,EM_SETSEL,n,n);
    SendMessageW(gLog,EM_REPLACESEL,FALSE,(LPARAM)(t+L"\r\n").c_str());
}
static void LogAsync(const std::wstring& t){ PostMessageW(gWnd,WM_LOG,0,(LPARAM)new std::wstring(t)); }
static void ProgAsync(int v){ PostMessageW(gWnd,WM_PROG,v,0); }
static void DoneAsync(bool ok,const std::wstring& m){ PostMessageW(gWnd,WM_DONE,ok?1:0,(LPARAM)new std::wstring(m)); }

static bool ReadAll(const std::wstring& p,std::vector<uint8_t>& out){
    FILE* f=_wfopen(p.c_str(),L"rb"); if(!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    out.resize(sz); if(sz>0) fread(out.data(),1,sz,f);
    fclose(f); return sz>0;
}
static bool SaveFile(const std::wstring& p,const std::vector<uint8_t>& d){
    HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD w=0; WriteFile(h,d.data(),(DWORD)d.size(),&w,nullptr); CloseHandle(h);
    return w==(DWORD)d.size();
}
static void CollectDir(const std::wstring& dir,std::vector<std::wstring>& out){
    std::error_code ec;
    for(auto& e:fs::recursive_directory_iterator(dir,
            fs::directory_options::skip_permission_denied,ec))
        if(e.is_regular_file(ec)) out.push_back(e.path().wstring());
}
static void RefreshList(){
    SendMessageW(gList,LB_RESETCONTENT,0,0);
    for(auto& f:gFiles){
        auto p=f.rfind(L'\\');
        std::wstring name=(p!=std::wstring::npos)?f.substr(p+1):f;
        SendMessageW(gList,LB_ADDSTRING,0,(LPARAM)name.c_str());
    }
}

// ── 訓練 ─────────────────────────────────────────────────────
struct TrainArgs {
    std::vector<std::wstring> files;
    std::wstring outL3,outL5;
    int maxPhrases,minFreq,minLen,maxLen;
};
static void RunTrain(TrainArgs a){
  try{
    auto t0=std::chrono::steady_clock::now();
    LogAsync(L""); LogAsync(L"=== 訓練開始 ===");
    wchar_t b[128];
    swprintf(b,128,L"  %dファイル / フレーズ最大:%d 最小頻度:%d 長さ:%d-%d",
        (int)a.files.size(),a.maxPhrases,a.minFreq,a.minLen,a.maxLen); LogAsync(b);
    ProgAsync(2);
    // Step1
    LogAsync(L"  [1/4] 読み込み中...");
    std::vector<std::vector<uint8_t>> docs;
    size_t totalBytes=0;
    for(int i=0;i<(int)a.files.size();i++){
        std::vector<uint8_t> d;
        if(ReadAll(a.files[i],d)){ totalBytes+=d.size(); docs.push_back(std::move(d)); }
        if(i%10==0) ProgAsync(2+8*i/(int)a.files.size());
    }
    swprintf(b,128,L"  %dファイル / %.1fKB",(int)docs.size(),(double)totalBytes/1024.0); LogAsync(b);
    ProgAsync(10);
    if(docs.empty()){ DoneAsync(false,L"読み込めるファイルがありません"); gBusy=false; return; }
    // Step2
    LogAsync(L"  [2/4] パターン分析中...");
    static constexpr size_t SAMPLE=64*1024;
    std::unordered_map<std::string,uint32_t> freq;
    freq.reserve(1<<18);
    for(int di=0;di<(int)docs.size();di++){
        auto& doc=docs[di];
        size_t end=std::min(doc.size(),SAMPLE);
        for(int len=a.minLen;len<=a.maxLen&&(size_t)len<=end;++len)
            for(size_t i=0;i+len<=end;++i)
                freq[std::string((const char*)doc.data()+i,len)]++;
        if(di%5==0) ProgAsync(10+25*di/(int)docs.size());
    }
    swprintf(b,128,L"  %zuパターン",(size_t)freq.size()); LogAsync(b);
    ProgAsync(35);
    // Step3
    LogAsync(L"  [3/4] フレーズ選定中...");
    struct Cand{ std::string s; uint32_t freq; double score; };
    std::vector<Cand> cands;
    for(auto& [k,v]:freq){
        if((int)v<a.minFreq||(int)k.size()<a.minLen||(int)k.size()>a.maxLen) continue;
        int pr=0; for(unsigned char c:k) if(c>=0x20) ++pr;
        if(pr<(int)k.size()/2) continue;
        cands.push_back({k,v,(double)v*(double)k.size()});
    }
    freq.clear();
    std::sort(cands.begin(),cands.end(),[](auto& a,auto& b){ return a.score>b.score; });
    std::vector<Cand> sel;
    for(auto& c:cands){
        if((int)sel.size()>=a.maxPhrases) break;
        bool skip=false;
        for(auto& s:sel){ if(s.s.size()>c.s.size()&&s.s.find(c.s)!=std::string::npos){ skip=true; break; } }
        if(!skip) sel.push_back(c);
    }
    swprintf(b,128,L"  %d候補 → %dフレーズ",(int)cands.size(),(int)sel.size()); LogAsync(b);
    LogAsync(L"  [上位フレーズ]:");
    for(int i=0;i<std::min(8,(int)sel.size());i++){
        std::wstring pre;
        for(size_t j=0;j<std::min((size_t)24,sel[i].s.size());j++){
            unsigned char c=(unsigned char)sel[i].s[j];
            pre+=(c>=0x20&&c<0x7F)?(wchar_t)c:L'.';
        }
        swprintf(b,128,L"    #%d [%zuB x%u] \"%s\"",i+1,sel[i].s.size(),sel[i].freq,pre.c_str());
        LogAsync(b);
    }
    ProgAsync(70);
    // Step4 L3
    bool savedL3=false,savedL5=false;
    if(!a.outL3.empty()){
        LogAsync(L"  [4/4] L3辞書保存...");
        std::vector<uint8_t> out;
        out.insert(out.end(),DICT_MAGIC,DICT_MAGIC+8); out.push_back(1);
        uint32_t n=(uint32_t)sel.size();
        for(int i=0;i<4;i++) out.push_back((n>>(i*8))&0xFF);
        for(auto& e:sel){
            uint16_t len=(uint16_t)e.s.size();
            out.push_back(len&0xFF); out.push_back((len>>8)&0xFF);
            out.insert(out.end(),e.s.begin(),e.s.end());
            for(int i=0;i<4;i++) out.push_back((e.freq>>(i*8))&0xFF);
        }
        if(SaveFile(a.outL3,out)){
            swprintf(b,128,L"  L3辞書 ✓  %dフレーズ / %.1fKB",(int)sel.size(),(double)out.size()/1024.0); LogAsync(b);
            savedL3=true;
        } else LogAsync(L"  L3辞書 保存失敗 (エラー="+std::to_wstring(GetLastError())+L")");
    }
    // Step4 L5
    if(!a.outL5.empty()){
        LogAsync(L"  [4/4] L5辞書保存...");
        static constexpr size_t L5_LIMIT=512*1024;
        auto hash64=[](const std::vector<uint8_t>& d)->uint64_t{
            uint64_t h=14695981039346656037ULL;
            for(auto b2:d){ h^=b2; h*=1099511628211ULL; } return h;
        };
        std::vector<uint8_t> out;
        out.insert(out.end(),L5P_MAGIC,L5P_MAGIC+8);
        uint32_t cnt=0; size_t cntpos=out.size();
        for(int i=0;i<4;i++) out.push_back(0);
        for(auto& doc:docs){
            if(doc.size()>L5_LIMIT) continue;
            uint64_t h=hash64(doc);
            for(int i=0;i<8;i++) out.push_back((h>>(i*8))&0xFF);
            uint64_t os=doc.size(); for(int i=0;i<8;i++) out.push_back((os>>(i*8))&0xFF);
            uint32_t ds=(uint32_t)doc.size(); for(int i=0;i<4;i++) out.push_back((ds>>(i*8))&0xFF);
            out.insert(out.end(),doc.begin(),doc.end()); cnt++;
        }
        for(int i=0;i<4;i++) out[cntpos+i]=(uint8_t)((cnt>>(i*8))&0xFF);
        if(SaveFile(a.outL5,out)){
            swprintf(b,128,L"  L5辞書 ✓  %uエントリ / %.1fKB",cnt,(double)out.size()/1024.0); LogAsync(b);
            savedL5=true;
        } else LogAsync(L"  L5辞書 保存失敗");
    }
    ProgAsync(100);
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    std::wstring result=L"=== 完了 ✓  ";
    if(savedL3) result+=L"L3("+std::to_wstring(sel.size())+L"フレーズ) ";
    if(savedL5) result+=L"L5 ";
    result+=std::to_wstring((int)ms)+L"ms ===";
    LogAsync(result); DoneAsync(true,result);
  }catch(std::exception& ex){ DoneAsync(false,L"エラー: "+std::wstring(ex.what(),ex.what()+strlen(ex.what()))); }
  catch(...){ DoneAsync(false,L"予期しないエラー"); }
  gBusy=false;
}

// ── ダイアログ ────────────────────────────────────────────────
static std::wstring BrowseSaveFile(const wchar_t* filter,const wchar_t* ext){
    wchar_t buf[MAX_PATH]={};
    OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=gWnd; o.lpstrFile=buf; o.nMaxFile=MAX_PATH;
    o.lpstrFilter=filter; o.lpstrDefExt=ext; o.Flags=OFN_OVERWRITEPROMPT;
    return GetSaveFileNameW(&o)?buf:L"";
}
static std::wstring BrowseDir(){
    IFileDialog* pfd=nullptr;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_ALL,
        IID_IFileOpenDialog,(void**)&pfd))) return L"";
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
static std::vector<std::wstring> BrowseFiles(){
    wchar_t buf[32768]={};
    OPENFILENAMEW o={sizeof(o)}; o.hwndOwner=gWnd; o.lpstrFile=buf; o.nMaxFile=32768;
    o.lpstrFilter=L"全てのファイル\0*.*\0\0";
    o.Flags=OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
    std::vector<std::wstring> result;
    if(!GetOpenFileNameW(&o)) return result;
    wchar_t* p=buf;
    std::wstring dir(p); p+=dir.size()+1;
    if(*p==0){ result.push_back(dir); return result; }
    while(*p){ std::wstring fn(p); result.push_back(dir+L"\\"+fn); p+=fn.size()+1; }
    return result;
}

// ── パネルWndProc (WM_COMMAND転送付き) ───────────────────────
static LRESULT CALLBACK PanelProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COMMAND) return SendMessageW(GetParent(h),WM_COMMAND,w,l);
    if(m==WM_ERASEBKGND){
        HBRUSH b=(h==gLeftPanel)?gBrBg1:gBrBg1;
        RECT r; GetClientRect(h,&r); FillRect((HDC)w,&r,b); return 1;
    }
    if(m==WM_PAINT){
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        FillRect(dc,&rc,h==gRightPanel?gBrBg1:gBrBg);
        if(h==gRightPanel){
            HPEN pen=CreatePen(PS_SOLID,1,C_BORDER); HPEN op=(HPEN)SelectObject(dc,pen);
            MoveToEx(dc,0,0,nullptr); LineTo(dc,0,rc.bottom);
            SelectObject(dc,op); DeleteObject(pen);
        }
        EndPaint(h,&ps); return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

// ── ヘッダーWndProc ───────────────────────────────────────────
static LRESULT CALLBACK TrainHeaderProc(HWND h,UINT m,WPARAM w,LPARAM l){
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
        TextOutW(dc,14,8,L"CPB Train",9); SelectObject(dc,of); DeleteObject(f);
        SetTextColor(dc,C_TEXT3);
        HFONT fs=CreateFontW(-10,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
        of=(HFONT)SelectObject(dc,fs);
        TextOutW(dc,108,13,L"辞書訓練アプリ",7); SelectObject(dc,of); DeleteObject(fs);
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

        // パネルクラス登録
        WNDCLASSEXW pc={sizeof(pc)};
        pc.lpfnWndProc=PanelProc; pc.hInstance=GetModuleHandleW(nullptr);
        pc.hbrBackground=nullptr; pc.lpszClassName=L"CPBTPanel";
        pc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        RegisterClassExW(&pc);
        // ヘッダークラス
        WNDCLASSEXW hc={sizeof(hc)};
        hc.lpfnWndProc=TrainHeaderProc; hc.hInstance=GetModuleHandleW(nullptr);
        hc.hbrBackground=nullptr; hc.lpszClassName=L"CPBTHeader";
        hc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        RegisterClassExW(&hc);

        auto mk=[&](HWND par,const wchar_t* cls,const wchar_t* t,DWORD st,
                    int x,int y,int w,int h,int id,HFONT f=nullptr)->HWND{
            HWND r=CreateWindowW(cls,t,WS_CHILD|WS_VISIBLE|st,x,y,w,h,par,(HMENU)(intptr_t)id,nullptr,nullptr);
            SendMessageW(r,WM_SETFONT,(WPARAM)(f?f:gFontUI),TRUE); return r; };

        int HDR_H=38;
        CreateWindowW(L"CPBTHeader",nullptr,WS_CHILD|WS_VISIBLE,0,0,700,HDR_H,hw,nullptr,nullptr,nullptr);

        int BY=HDR_H;
        int LP_W=440, RP_W=240; // 左パネル幅, 右パネル幅

        // ─ 左パネル: サンプルリスト ─
        gLeftPanel=CreateWindowW(L"CPBTPanel",nullptr,WS_CHILD|WS_VISIBLE,0,BY,LP_W,560,hw,nullptr,nullptr,nullptr);
        {
            auto mp=[&](const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr){
                return mk(gLeftPanel,cls,t,st,x,y,w,h,id,f); };
            // セクションラベル
            SetBkMode(GetDC(gLeftPanel),TRANSPARENT);
            mp(L"STATIC",L"SAMPLES",SS_LEFT,10,8,200,14,0,gFontSmall);
            // リスト
            gList=CreateWindowW(L"LISTBOX",nullptr,
                WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,
                10,24,LP_W-20,180,gLeftPanel,(HMENU)ID_LIST,nullptr,nullptr);
            SendMessageW(gList,WM_SETFONT,(WPARAM)gFontMono,TRUE);
            // ボタン行
            mk(gLeftPanel,L"BUTTON",L"📁 フォルダ追加",BS_PUSHBUTTON,10, 208,(LP_W-30)/2,28,ID_BTN_ADDDIR, gFontBold);
            mk(gLeftPanel,L"BUTTON",L"📄 ファイル追加",BS_PUSHBUTTON,(LP_W-20)/2+10,208,(LP_W-30)/2,28,ID_BTN_ADDFILE,gFontUI);
            mk(gLeftPanel,L"BUTTON",L"✕ クリア",        BS_PUSHBUTTON,10,240,80,24,ID_BTN_CLEAR);

            // 出力辞書セクション
            mp(L"STATIC",L"OUTPUT",SS_LEFT,10,276,200,14,0,gFontSmall);
            mp(L"STATIC",L"L3辞書 (.cpbdict):",SS_LEFT,10,294,140,16,0,gFontBold);
            gOutL3=mp(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,10,310,LP_W-68,24,ID_OUT_L3);
            mk(gLeftPanel,L"BUTTON",L"保存先",BS_PUSHBUTTON,LP_W-56,310,48,24,ID_BTN_BRL3);
            mp(L"STATIC",L"L5辞書 (.dict):",SS_LEFT,10,338,120,16,0,gFontBold);
            gOutL5=mp(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,10,354,LP_W-68,24,ID_OUT_L5);
            mk(gLeftPanel,L"BUTTON",L"保存先",BS_PUSHBUTTON,LP_W-56,354,48,24,ID_BTN_BRL5);
            mp(L"STATIC",L"空欄はスキップ / どちらか一方だけでもOK",SS_LEFT,10,382,LP_W-20,14,0,gFontSmall);

            // プログレス + ログ
            gProg=CreateWindowW(PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE,
                10,400,LP_W-20,10,gLeftPanel,(HMENU)ID_PROG,nullptr,nullptr);
            SendMessageW(gProg,PBM_SETRANGE,0,MAKELPARAM(0,100));
            gLog=CreateWindowW(L"EDIT",nullptr,
                WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
                10,414,LP_W-20,130,gLeftPanel,(HMENU)ID_LOG,nullptr,nullptr);
            SendMessageW(gLog,WM_SETFONT,(WPARAM)gFontMono,TRUE);
        }

        // ─ 右パネル: パラメータ + 学習ボタン ─
        gRightPanel=CreateWindowW(L"CPBTPanel",nullptr,WS_CHILD|WS_VISIBLE,LP_W,BY,RP_W,560,hw,nullptr,nullptr,nullptr);
        {
            auto rp=[&](const wchar_t* cls,const wchar_t* t,DWORD st,int x,int y,int w,int h,int id,HFONT f=nullptr){
                return mk(gRightPanel,cls,t,st,x,y,w,h,id,f); };
            int y=10;
            rp(L"STATIC",L"PARAMETERS",SS_LEFT,14,y,180,14,0,gFontSmall); y+=20;

            auto paramRow=[&](const wchar_t* label,const wchar_t* val,int id,int& y)->HWND{
                rp(L"STATIC",label,SS_LEFT,14,y+3,130,16,0);
                HWND e=rp(L"EDIT",val,WS_BORDER|ES_NUMBER,148,y,RP_W-162,22,id,gFontUI);
                y+=28; return e;
            };
            gEdPhrases=paramRow(L"最大フレーズ数",L"1024",ID_EDIT_PHRASES,y);
            gEdMinFreq =paramRow(L"最小頻度",     L"3",   ID_EDIT_MINFREQ, y);
            gEdMinLen  =paramRow(L"最小長 (B)",   L"4",   ID_EDIT_MINLEN,  y);
            gEdMaxLen  =paramRow(L"最大長 (B)",   L"32",  ID_EDIT_MAXLEN,  y);
            y+=8;

            // パラメータの説明
            rp(L"STATIC",
                L"フレーズ数が多いほど高圧縮\n"
                L"→ 辞書ファイルが大きくなる\n\n"
                L"最小頻度: サンプル中で何回\n"
                L"以上出現したら採用するか\n\n"
                L"長さ: 短すぎると効果薄、\n"
                L"長すぎるとヒット率が低下",
                SS_LEFT,14,y,RP_W-24,120,0,gFontSmall);
            y+=130;

            // 使い方
            rp(L"STATIC",L"USAGE",SS_LEFT,14,y,180,14,0,gFontSmall); y+=18;
            rp(L"STATIC",
                L"1. フォルダ/ファイルを追加\n"
                L"2. 出力先を指定\n"
                L"3. 学習スタート\n\n"
                L"生成した辞書を\n"
                L"CPB GUI の辞書欄に指定",
                SS_LEFT,14,y,RP_W-24,100,0,gFontSmall);
            y+=110;

            // 学習スタートボタン
            gBtnTrain=rp(L"BUTTON",L"▶▶  学習スタート",
                BS_PUSHBUTTON,14,y,RP_W-28,38,ID_BTN_TRAIN,gFontBold);
        }

        DragAcceptFiles(hw,TRUE);
        AppendLog(L"CPB 辞書訓練アプリ");
        AppendLog(L"左: サンプルファイルを追加 → 出力先を指定 → 学習スタート");
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp,C_BG); SetTextColor((HDC)wp,C_TEXT); return (LRESULT)gBrBg;
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id==ID_BTN_ADDDIR){
            auto dir=BrowseDir();
            if(!dir.empty()){
                int before=(int)gFiles.size();
                CollectDir(dir,gFiles); RefreshList();
                wchar_t b[64]; swprintf(b,64,L"%dファイルを追加",(int)gFiles.size()-before);
                AppendLog(b);
            }
            return 0;
        }
        if(id==ID_BTN_ADDFILE){
            auto files=BrowseFiles();
            for(auto& f:files) gFiles.push_back(f);
            RefreshList();
            wchar_t b[64]; swprintf(b,64,L"%dファイルを追加",(int)files.size());
            AppendLog(b); return 0;
        }
        if(id==ID_BTN_CLEAR){
            gFiles.clear(); SendMessageW(gList,LB_RESETCONTENT,0,0);
            AppendLog(L"クリアしました"); return 0;
        }
        if(id==ID_BTN_BRL3){
            auto p=BrowseSaveFile(L"L3辞書\0*.cpbdict\0全てのファイル\0*.*\0\0",L"cpbdict");
            if(!p.empty()) SetWindowTextW(gOutL3,p.c_str()); return 0;
        }
        if(id==ID_BTN_BRL5){
            auto p=BrowseSaveFile(L"L5辞書\0*.dict\0全てのファイル\0*.*\0\0",L"dict");
            if(!p.empty()) SetWindowTextW(gOutL5,p.c_str()); return 0;
        }
        if(id==ID_BTN_TRAIN){
            if(gBusy) return 0;
            if(gFiles.empty()){
                MessageBoxW(hw,L"左パネルにサンプルファイルを追加してください",
                    L"CPB Train",MB_OK|MB_ICONWARNING); return 0;
            }
            std::wstring outL3=GetT(gOutL3), outL5=GetT(gOutL5);
            // 両方空なら自動生成
            if(outL3.empty()&&outL5.empty()){
                std::wstring base=gFiles[0];
                auto sl=base.rfind(L'\\');
                std::wstring dir=(sl!=std::wstring::npos)?base.substr(0,sl):L".";
                SYSTEMTIME st; GetLocalTime(&st);
                wchar_t ts[32]; swprintf(ts,32,L"%04d%02d%02d_%02d%02d%02d",
                    st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
                outL3=dir+L"\\cpb_dict_"+ts+L".cpbdict";
                outL5=dir+L"\\cpb_dict_"+ts+L".dict";
                SetWindowTextW(gOutL3,outL3.c_str());
                SetWindowTextW(gOutL5,outL5.c_str());
                AppendLog(L"[自動] 出力先を設定しました");
            }
            TrainArgs a; a.files=gFiles; a.outL3=outL3; a.outL5=outL5;
            a.maxPhrases=GetI(gEdPhrases,1024); a.minFreq=GetI(gEdMinFreq,3);
            a.minLen=GetI(gEdMinLen,4); a.maxLen=GetI(gEdMaxLen,32);
            gBusy=true; EnableWindow(gBtnTrain,FALSE);
            SetWindowTextW(hw,L"CPB Train — 訓練中...");
            std::thread(RunTrain,a).detach();
            return 0;
        }
        return 0;
    }
    case WM_DROPFILES:{
        HDROP hd=(HDROP)wp; wchar_t buf[MAX_PATH];
        UINT n=DragQueryFileW(hd,0xFFFFFFFF,nullptr,0);
        for(UINT i=0;i<n;i++){
            DragQueryFileW(hd,i,buf,MAX_PATH);
            DWORD attr=GetFileAttributesW(buf);
            if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY))
                CollectDir(buf,gFiles);
            else gFiles.push_back(buf);
        }
        DragFinish(hd); RefreshList();
        wchar_t b[64]; swprintf(b,64,L"合計 %d ファイル",(int)gFiles.size());
        AppendLog(b); return 0;
    }
    case WM_LOG:{ auto* t=(std::wstring*)lp; AppendLog(*t); delete t; return 0; }
    case WM_PROG: SendMessageW(gProg,PBM_SETPOS,(int)wp,0); return 0;
    case WM_DONE:{
        auto* t=(std::wstring*)lp; std::wstring m=*t; delete t;
        bool ok=(wp!=0);
        EnableWindow(gBtnTrain,TRUE); gBusy=false;
        SetWindowTextW(hw,ok?L"CPB Train — 完了 ✓":L"CPB 辞書訓練アプリ");
        if(ok) MessageBoxW(hw,
            (m+L"\n\n生成した辞書を CPB GUI の辞書欄に指定してください。").c_str(),
            L"訓練完了",MB_OK|MB_ICONINFORMATION);
        else{ AppendLog(L"[エラー] "+m); MessageBoxW(hw,m.c_str(),L"エラー",MB_OK|MB_ICONERROR); }
        return 0;
    }
    case WM_SIZE:{
        RECT rc; GetClientRect(hw,&rc);
        int w=rc.right,h=rc.bottom;
        int LP_W=w-240, RP_W=240, BY=38;
        if(gLeftPanel)  SetWindowPos(gLeftPanel, nullptr,0,   BY,LP_W,h-BY,SWP_NOZORDER);
        if(gRightPanel) SetWindowPos(gRightPanel,nullptr,LP_W,BY,RP_W,h-BY,SWP_NOZORDER);
        // 左パネル内のコントロールをリサイズ
        HWND brl3=(HWND)GetDlgItem(gLeftPanel,ID_BTN_BRL3);
        HWND brl5=(HWND)GetDlgItem(gLeftPanel,ID_BTN_BRL5);
        int fw=LP_W-20;
        if(gList)  SetWindowPos(gList, nullptr,10,24,fw,180,SWP_NOZORDER);
        // adddir/addfile ボタンを半分ずつ
        HWND badd=(HWND)GetDlgItem(gLeftPanel,ID_BTN_ADDDIR);
        HWND bfile=(HWND)GetDlgItem(gLeftPanel,ID_BTN_ADDFILE);
        if(badd)  SetWindowPos(badd, nullptr,10,208,(fw-8)/2,28,SWP_NOZORDER);
        if(bfile) SetWindowPos(bfile,nullptr,10+(fw-8)/2+8,208,(fw-8)/2,28,SWP_NOZORDER);
        if(gOutL3)SetWindowPos(gOutL3,nullptr,10,310,fw-58,24,SWP_NOZORDER);
        if(brl3)  SetWindowPos(brl3,  nullptr,fw-46,310,48,24,SWP_NOZORDER);
        if(gOutL5)SetWindowPos(gOutL5,nullptr,10,354,fw-58,24,SWP_NOZORDER);
        if(brl5)  SetWindowPos(brl5,  nullptr,fw-46,354,48,24,SWP_NOZORDER);
        if(gProg) SetWindowPos(gProg, nullptr,10,400,fw,10,SWP_NOZORDER);
        if(gLog)  SetWindowPos(gLog,  nullptr,10,414,fw,h-BY-414-8,SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO:{ auto* m=(MINMAXINFO*)lp; m->ptMinTrackSize={640,560}; return 0; }
    case WM_DESTROY:
        DeleteObject(gFontUI); DeleteObject(gFontBold); DeleteObject(gFontMono); DeleteObject(gFontSmall);
        DeleteObject(gBrBg); DeleteObject(gBrBg1); DeleteObject(gBrBg2);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int ns){
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc={sizeof(wc)};
    wc.lpfnWndProc=WndProc; wc.hInstance=hi;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=L"CPBTrain";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);
    gWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"CPBTrain",
        L"CPB 辞書訓練アプリ",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,680,560,
        nullptr,nullptr,hi,nullptr);
    ShowWindow(gWnd,ns); UpdateWindow(gWnd);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return (int)msg.wParam;
}
