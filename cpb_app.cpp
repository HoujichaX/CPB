// cpb_app.cpp ── CPB 独立アプリ (WebView2 + 内蔵UI)
//
// ビルド方法 (VS Developer Command Prompt):
//   1. NuGet で Microsoft.Web.WebView2 を取得:
//      nuget install Microsoft.Web.WebView2 -OutputDirectory packages
//
//   2. コンパイル:
//      cl /std:c++17 /O2 /MT /EHsc /DWIN32 /DUNICODE /D_UNICODE
//         /DCPB_NO_ZSTD /DNO_FRAME_IO
//         /I"packages\Microsoft.Web.WebView2.1.0.2739.15\build\native\include"
//         cpb_app.cpp
//         /link /SUBSYSTEM:WINDOWS
//         "packages\Microsoft.Web.WebView2.1.0.2739.15\build\native\x64\WebView2LoaderStatic.lib"
//         user32.lib gdi32.lib ole32.lib oleaut32.lib shlwapi.lib version.lib
//
//   または build_app.bat を実行
//
// 成果物: cpb_app.exe 単体で動作 (依存なし、インストール不要)
//
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <functional>
#include <filesystem>
#include <chrono>
#include <iomanip>
using Microsoft::WRL::Callback;

// ── CPB Core ─────────────────────────────────────────────────
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
#include "cpb_dict_protocol.cpp"
#include "dict_evolution.cpp"
#include "layer_pipeline.cpp"
#include "search_api.cpp"
#include "dict_share.cpp"

// ── 埋め込みHTML/CSS/JS ───────────────────────────────────────
// このHTMLがそのままUIになる。外部ファイル不要。
static const char* UI_HTML = R"HTML(
<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CPB</title>
<style>
  :root {
    --bg: #1a1610; --surface: #252018; --surface2: #2e2820;
    --border: #4a3e2e; --accent: #c8a060; --accent2: #8B7040;
    --text: #e8d8b8; --text2: #a09070; --text3: #706040;
    --green: #70c870; --blue: #70a8e8; --red: #e87070;
    --log-bg: #0e0c08;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Meiryo', 'Yu Gothic UI', sans-serif;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    -webkit-app-region: no-drag;
    user-select: none;
  }

  /* タイトルバー */
  .titlebar {
    background: #0e0c08;
    border-bottom: 1px solid var(--border);
    padding: 0 16px;
    display: flex;
    align-items: center;
    height: 42px;
    -webkit-app-region: drag;
    flex-shrink: 0;
  }
  .titlebar-icon { font-size: 18px; margin-right: 8px; }
  .titlebar-title { font-size: 13px; color: var(--accent); font-weight: 500; letter-spacing: 0.5px; }
  .titlebar-sub   { font-size: 11px; color: var(--text3); margin-left: 10px; }

  /* メイン */
  .main { padding: 16px; display: flex; flex-direction: column; gap: 12px; flex: 1; }

  /* ドロップゾーン */
  .dropzone {
    border: 1.5px dashed var(--border);
    border-radius: 8px;
    padding: 14px;
    text-align: center;
    color: var(--text3);
    font-size: 13px;
    background: var(--surface);
    transition: all 0.15s;
    cursor: default;
  }
  .dropzone.hover { border-color: var(--accent); background: var(--surface2); color: var(--text2); }
  .dropzone-icon { font-size: 22px; display: block; margin-bottom: 4px; }

  /* フォーム */
  .form-group { display: flex; flex-direction: column; gap: 6px; }
  .form-row { display: flex; align-items: center; gap: 8px; }
  .form-label { font-size: 11px; color: var(--text2); min-width: 46px; text-align: right; }
  .form-input {
    flex: 1; height: 28px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 5px;
    color: var(--text);
    padding: 0 8px;
    font-size: 12px;
    font-family: 'Consolas', monospace;
    outline: none;
    transition: border-color 0.1s;
  }
  .form-input:focus { border-color: var(--accent2); }
  .form-input::placeholder { color: var(--text3); }
  .btn-browse {
    padding: 0 10px; height: 28px;
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 5px;
    color: var(--text2);
    font-size: 11px;
    cursor: pointer;
    transition: all 0.1s;
    white-space: nowrap;
  }
  .btn-browse:hover { border-color: var(--accent2); color: var(--text); }

  /* セパレーター */
  .sep { height: 1px; background: var(--border); opacity: 0.5; }

  /* プロファイルと設定 */
  .settings-row { display: flex; gap: 10px; align-items: center; }
  .select-wrap { flex: 1; position: relative; }
  .form-select {
    width: 100%; height: 28px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 5px;
    color: var(--text);
    padding: 0 8px;
    font-size: 12px;
    outline: none;
    cursor: pointer;
    appearance: none;
    -webkit-appearance: none;
  }
  .select-arrow {
    position: absolute; right: 8px; top: 50%;
    transform: translateY(-50%);
    color: var(--text3); font-size: 10px; pointer-events: none;
  }
  .chk-wrap { display: flex; align-items: center; gap: 6px; cursor: pointer; white-space: nowrap; }
  .chk-wrap input { accent-color: var(--accent); cursor: pointer; }
  .chk-label { font-size: 12px; color: var(--text2); }

  /* ボタン */
  .actions { display: flex; gap: 8px; align-items: center; }
  .btn {
    height: 34px; padding: 0 16px;
    border-radius: 6px;
    font-size: 13px; font-weight: 600;
    cursor: pointer; border: none;
    transition: all 0.1s;
    letter-spacing: 0.3px;
  }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn-pack {
    background: linear-gradient(135deg, #c8a060, #a07840);
    color: #0e0c08;
    box-shadow: 0 2px 8px rgba(200,160,96,0.3);
  }
  .btn-pack:hover:not(:disabled) { background: linear-gradient(135deg, #d8b070, #b08850); }
  .btn-unpack {
    background: linear-gradient(135deg, #60a870, #407850);
    color: #0e0c08;
    box-shadow: 0 2px 8px rgba(96,168,112,0.3);
  }
  .btn-unpack:hover:not(:disabled) { background: linear-gradient(135deg, #70b880, #508860); }
  .btn-search {
    background: linear-gradient(135deg, #6088c8, #405898);
    color: #e8e8f8;
    box-shadow: 0 2px 8px rgba(96,136,200,0.3);
  }
  .btn-search:hover:not(:disabled) { background: linear-gradient(135deg, #7098d8, #5068a8); }
  .btn-clear {
    margin-left: auto;
    background: transparent;
    border: 1px solid var(--border);
    border-radius: 5px;
    color: var(--text3);
    font-size: 11px;
    height: 28px;
    padding: 0 10px;
    cursor: pointer;
  }
  .btn-clear:hover { color: var(--text2); border-color: var(--accent2); }
  .query-input {
    flex: 1;
    height: 28px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 5px;
    color: var(--text);
    padding: 0 8px;
    font-size: 12px;
    outline: none;
  }
  .query-input:focus { border-color: var(--accent2); }

  /* プログレス */
  .progress-wrap {
    height: 4px;
    background: var(--surface2);
    border-radius: 2px;
    overflow: hidden;
  }
  .progress-fill {
    height: 100%;
    border-radius: 2px;
    background: linear-gradient(90deg, var(--accent2), var(--accent));
    width: 0%;
    transition: width 0.3s ease;
  }

  /* ログ */
  .log-area {
    background: var(--log-bg);
    border: 1px solid #2a2010;
    border-radius: 8px;
    padding: 10px 12px;
    flex: 1;
    min-height: 160px;
    overflow-y: auto;
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 11.5px;
    line-height: 1.7;
    scrollbar-width: thin;
    scrollbar-color: var(--border) transparent;
  }
  .log-area::-webkit-scrollbar { width: 4px; }
  .log-area::-webkit-scrollbar-thumb { background: var(--border); border-radius: 2px; }
  .l-default { color: #706050; }
  .l-header  { color: var(--accent); font-weight: 600; }
  .l-info    { color: #80c080; }
  .l-stage   { color: #6090c0; }
  .l-hash    { color: #c0a040; }
  .l-done    { color: #90e890; font-weight: 600; }
  .l-error   { color: var(--red); font-weight: 600; }
  .l-warn    { color: #e8c060; }

  /* ステータスバー */
  .statusbar {
    background: #0e0c08;
    border-top: 1px solid var(--border);
    padding: 5px 16px;
    font-size: 11px;
    color: var(--text3);
    display: flex; align-items: center; gap: 8px;
    flex-shrink: 0;
  }
  .status-dot { width: 6px; height: 6px; border-radius: 50%; }
  .dot-ready { background: var(--accent2); }
  .dot-busy  { background: var(--accent); animation: pulse 0.8s infinite; }
  .dot-done  { background: var(--green); }
  .dot-error { background: var(--red); }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.4} }
</style>
</head>
<body>

<!-- タイトルバー -->
<div class="titlebar">
  <span class="titlebar-icon">🍫</span>
  <span class="titlebar-title">CPB — Cocoa Powder Bottle</span>
  <span class="titlebar-sub">Invisible Data Architecture</span>
</div>

<!-- メイン -->
<div class="main">

  <!-- ドロップゾーン -->
  <div class="dropzone" id="dropzone"
       ondragover="event.preventDefault();this.classList.add('hover')"
       ondragleave="this.classList.remove('hover')"
       ondrop="handleDrop(event)">
    <span class="dropzone-icon">📂</span>
    ドラッグ&amp;ドロップ — または下の「参照」で選択
  </div>

  <!-- フォーム -->
  <div class="form-group">
    <div class="form-row">
      <span class="form-label">入力</span>
      <input class="form-input" id="inPath" placeholder="入力ファイルパス" oninput="autoFillOut()">
      <button class="btn-browse" onclick="browseFile('open','inPath')">参照...</button>
    </div>
    <div class="form-row">
      <span class="form-label">出力</span>
      <input class="form-input" id="outPath" placeholder="出力ファイルパス">
      <button class="btn-browse" onclick="browseFile('save','outPath')">参照...</button>
    </div>
    <div class="form-row">
      <span class="form-label">辞書</span>
      <input class="form-input" id="dictPath" placeholder="辞書ファイル (--learning 時)">
      <button class="btn-browse" onclick="browseFile('dict','dictPath')">参照...</button>
      <label class="chk-wrap">
        <input type="checkbox" id="chkLearn">
        <span class="chk-label">学習モード</span>
      </label>
    </div>
  </div>

  <div class="sep"></div>

  <!-- プロファイル -->
  <div class="settings-row">
    <span class="form-label" style="min-width:60px">プロファイル</span>
    <div class="select-wrap">
      <select class="form-select" id="selProfile">
        <option value="0">STANDARD — 汎用 (圧縮+保護+検索)</option>
        <option value="1">ARCHIVE  — 長期保存 (保護優先)</option>
        <option value="2">STEGO    — 最小サイズ (検索なし)</option>
        <option value="3">DEFENSE  — 耐タンパー (多重保護)</option>
        <option value="4">AI       — AI間通信向け</option>
        <option value="5">LEARN    — 学習モード</option>
      </select>
      <span class="select-arrow">▼</span>
    </div>
  </div>

  <!-- アクション -->
  <div class="actions">
    <button class="btn btn-pack"   id="btnPack"   onclick="doOp('pack')">PACK ▶</button>
    <button class="btn btn-unpack" id="btnUnpack" onclick="doOp('unpack')">UNPACK ◀</button>
    <button class="btn btn-search" id="btnSearch" onclick="doOp('search')">SEARCH</button>
    <input class="query-input" id="queryInput" placeholder="検索ワード">
    <button class="btn-clear" onclick="clearLog()">ログ消去</button>
  </div>

  <!-- プログレス -->
  <div class="progress-wrap">
    <div class="progress-fill" id="progress"></div>
  </div>

  <!-- ログ -->
  <div class="log-area" id="log"></div>
</div>

<!-- ステータスバー -->
<div class="statusbar">
  <div class="status-dot dot-ready" id="statusDot"></div>
  <span id="statusText">準備完了</span>
</div>

<script>
let busy = false;

function log(msg, cls='l-default') {
  const el = document.getElementById('log');
  const line = document.createElement('div');
  line.className = cls;
  line.textContent = msg;
  el.appendChild(line);
  el.scrollTop = el.scrollHeight;
}

function clearLog() { document.getElementById('log').innerHTML = ''; }

function setProgress(pct) {
  document.getElementById('progress').style.width = pct + '%';
}

function setStatus(msg, state='ready') {
  document.getElementById('statusText').textContent = msg;
  const dot = document.getElementById('statusDot');
  dot.className = 'status-dot dot-' + state;
}

function setBusy(b) {
  busy = b;
  ['btnPack','btnUnpack','btnSearch'].forEach(id => {
    document.getElementById(id).disabled = b;
  });
  setStatus(b ? '処理中...' : '完了', b ? 'busy' : 'done');
}

function autoFillOut() {
  const inp = document.getElementById('inPath').value;
  const out = document.getElementById('outPath');
  if (out.value === '' && inp) {
    const dot = inp.lastIndexOf('.');
    out.value = (dot >= 0 ? inp.substring(0, dot) : inp) + '.raw';
  }
}

function handleDrop(e) {
  e.preventDefault();
  document.getElementById('dropzone').classList.remove('hover');
  const files = e.dataTransfer.files;
  if (files.length > 0) {
    // WebView2経由でホストにファイルパスを渡す
    window.chrome.webview.postMessage({
      type: 'drop',
      path: files[0].name  // WebView2ではpath直接取れない → ホスト側でハンドル
    });
  }
}

function browseFile(mode, targetId) {
  window.chrome.webview.postMessage({ type: 'browse', mode, targetId });
}

function doOp(op) {
  if (busy) return;
  const inPath  = document.getElementById('inPath').value.trim();
  const outPath = document.getElementById('outPath').value.trim();
  const dictPath= document.getElementById('dictPath').value.trim();
  const profile = document.getElementById('selProfile').value;
  const learning= document.getElementById('chkLearn').checked;
  const query   = document.getElementById('queryInput').value.trim();

  log('');
  setBusy(true);
  setProgress(0);

  window.chrome.webview.postMessage({
    type: 'op', op, inPath, outPath, dictPath, profile, learning, query
  });
}

// ── ホストからのメッセージ受信 ──────────────────────────────
window.chrome.webview.addEventListener('message', e => {
  const msg = e.data;
  switch(msg.type) {
    case 'log':
      log(msg.text, msg.cls || 'l-default');
      break;
    case 'progress':
      setProgress(msg.value);
      break;
    case 'done':
      setBusy(false);
      setProgress(100);
      setStatus(msg.text, msg.ok ? 'done' : 'error');
      setTimeout(() => setProgress(0), 2000);
      break;
    case 'setPath':
      document.getElementById(msg.targetId).value = msg.path;
      if (msg.targetId === 'inPath') autoFillOut();
      break;
    case 'error':
      log('[ERROR] ' + msg.text, 'l-error');
      setBusy(false);
      setStatus(msg.text, 'error');
      break;
  }
});

// 起動メッセージ
log('🍫 CPB — Cocoa Powder Bottle', 'l-header');
log('   Pack any data as fine as powder. Store it where no one looks.', 'l-default');
log('', 'l-default');
log('   ドラッグ&ドロップでファイルを指定し、PACKボタンで圧縮します。', 'l-default');
</script>
</body>
</html>
)HTML";

// ── グローバル ────────────────────────────────────────────────
static HWND          g_hwnd     = nullptr;
static wil::com_ptr<ICoreWebView2Controller> g_wvCtrl;
static wil::com_ptr<ICoreWebView2>           g_wv;
static std::atomic<bool> g_busy{false};
static std::wstring  g_userDataDir;

// ── JSON エスケープ ───────────────────────────────────────────
static std::wstring json_esc(const std::wstring& s) {
    std::wstring r; r.reserve(s.size() + 16);
    for (wchar_t c : s) {
        switch(c) {
        case L'"': r += L"\\\""; break;
        case L'\\':r += L"\\\\"; break;
        case L'\n':r += L"\\n";  break;
        case L'\r':r += L"\\r";  break;
        default:   r += c;       break;
        }
    }
    return r;
}
static std::wstring json_esc(const std::string& s) {
    return json_esc(std::wstring(s.begin(), s.end()));
}

// ── WebView2 へメッセージ送信 ────────────────────────────────
static void wv_send(const std::wstring& json) {
    if (g_wv) g_wv->PostWebMessageAsJson(json.c_str());
}
static void wv_log(const std::wstring& text, const std::wstring& cls = L"l-default") {
    wv_send(L"{\"type\":\"log\",\"text\":\"" + json_esc(text) + L"\",\"cls\":\"" + cls + L"\"}");
}
static void wv_log(const std::string& text, const std::wstring& cls = L"l-default") {
    wv_log(std::wstring(text.begin(), text.end()), cls);
}
static void wv_progress(int pct) {
    wv_send(L"{\"type\":\"progress\",\"value\":" + std::to_wstring(pct) + L"}");
}
static void wv_done(const std::wstring& text, bool ok) {
    wv_send(L"{\"type\":\"done\",\"text\":\"" + json_esc(text) + L"\",\"ok\":" + (ok?L"true":L"false") + L"}");
}

// ── ファイルダイアログ ────────────────────────────────────────
static std::wstring browse_dialog(HWND hwnd, bool save,
    const wchar_t* filter = L"All Files\0*.*\0CPB Container\0*.raw;*.cpb\0JSON\0*.json\0\0")
{
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))
        return buf;
    return {};
}

// ── Wstr/str 変換 ─────────────────────────────────────────────
static std::string ws2s(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,ws.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string s(n-1,'\0');
    WideCharToMultiByte(CP_UTF8,0,ws.c_str(),-1,s.data(),n,nullptr,nullptr);
    return s;
}

// ── CPB 処理（ワーカースレッド）──────────────────────────────
struct OpArgs {
    std::string op, inPath, outPath, dictPath, query;
    int         profile;
    bool        learning;
};

static LayerPipeline make_pipeline(int idx) {
    switch(idx) {
    case 0: return LayerPipeline::standard();
    case 1: return LayerPipeline::archive();
    case 2: return LayerPipeline::stego();
    case 3: return LayerPipeline::defense();
    case 4: return LayerPipeline::ai_packet();
    case 5: return LayerPipeline::learn();
    default:return LayerPipeline::standard();
    }
}

static void run_op(OpArgs a) {
    auto t0 = std::chrono::high_resolution_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double,std::milli>(
            std::chrono::high_resolution_clock::now()-t0).count();
    };

    // ── PACK ─────────────────────────────────────────────────
    if (a.op == "pack") {
        wv_log("[PACK] reading: " + a.inPath);
        std::ifstream fin(a.inPath, std::ios::binary);
        if (!fin) { wv_done(L"ファイルが開けません", false); g_busy=false; return; }
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>());

        wv_log("[PACK] input: " + std::to_string(data.size()) + " bytes");
        wv_progress(15);

        CPBConfig cfg; cfg.learning = a.learning;
        if (a.learning && !a.dictPath.empty()) {
            if (l5_cache_load(a.dictPath))
                wv_log("[PACK] dict loaded: " + a.dictPath + " (" +
                       std::to_string(l5_learn_cache_size()) + " entries)");
        }

        auto pl  = make_pipeline(a.profile);
        auto enc = run_pipeline_encode(data, pl, cfg);
        if (!enc.success) { wv_done(L"encode 失敗", false); g_busy=false; return; }

        wv_progress(60);
        for (auto& sl : enc.ctx.stage_log) {
            char buf[80];
            snprintf(buf,sizeof(buf),"  %-18s %6zu → %6zu bytes",
                     layer_name(sl.layer), sl.size_before, sl.size_after);
            wv_log(buf, L"l-stage");
        }

        auto be = make_backend(BackendType::RAW);
        VideoFrame vf; vf.frame_id=0; vf.data=enc.data;
        be->write_frame(vf);
        auto container = be->serialize();

        std::ofstream fout(a.outPath, std::ios::binary);
        if (!fout) { wv_done(L"出力が書けません", false); g_busy=false; return; }
        fout.write((const char*)container.data(), container.size());

        wv_progress(85);
        uint8_t hash[32]; compute_hash(data,hash);
        std::ostringstream ss; ss << "[PACK] sha256: ";
        for(int i=0;i<8;++i) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)hash[i];
        ss<<"...";
        wv_log(ss.str(), L"l-hash");

        if (a.learning && !a.dictPath.empty())
            if (l5_cache_save(a.dictPath))
                wv_log("[PACK] dict saved (" + std::to_string(l5_learn_cache_size()) + " entries)");

        wv_progress(100);
        double r = (double)container.size()/data.size()*100;
        char summ[128]; snprintf(summ,sizeof(summ),
            "[PACK] done ✓  %zu → %zu bytes  ratio=%.1f%%  %.1fms",
            data.size(), container.size(), r, elapsed());
        wv_log(summ, L"l-done");
        wv_done(std::wstring(summ,summ+strlen(summ)), true);
    }

    // ── UNPACK ───────────────────────────────────────────────
    else if (a.op == "unpack") {
        wv_log("[UNPACK] reading: " + a.inPath);
        std::ifstream fin(a.inPath, std::ios::binary);
        if (!fin) { wv_done(L"ファイルが開けません", false); g_busy=false; return; }
        std::vector<uint8_t> container(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>());

        wv_progress(20);
        auto be = make_backend(BackendType::RAW);
        if (!be->deserialize(container)) {
            wv_done(L"コンテナ解析失敗", false); g_busy=false; return;
        }
        auto vf = be->read_frame(0);

        CPBConfig cfg; cfg.learning = a.learning;
        if (a.learning && !a.dictPath.empty())
            if (l5_cache_load(a.dictPath))
                wv_log("[UNPACK] dict loaded: " + a.dictPath);

        wv_progress(40);
        auto pl  = make_pipeline(a.profile);
        auto dec = run_pipeline_decode(vf.data, pl, cfg);

        for (auto& sl : dec.ctx.stage_log) {
            char buf[80];
            snprintf(buf,sizeof(buf),"  %-18s %6zu → %6zu bytes",
                     layer_name(sl.layer), sl.size_before, sl.size_after);
            wv_log(buf, L"l-stage");
        }
        wv_progress(80);

        if (!dec.success) { wv_done(L"decode 失敗: " + std::wstring(dec.error.begin(),dec.error.end()), false); g_busy=false; return; }

        std::ofstream fout(a.outPath, std::ios::binary);
        if (!fout) { wv_done(L"出力が書けません", false); g_busy=false; return; }
        fout.write((const char*)dec.data.data(), dec.data.size());

        wv_progress(100);
        char summ[128]; snprintf(summ,sizeof(summ),
            "[UNPACK] done ✓  %zu bytes  %.1fms", dec.data.size(), elapsed());
        wv_log(summ, L"l-done");
        wv_done(std::wstring(summ,summ+strlen(summ)), true);
    }

    // ── SEARCH ───────────────────────────────────────────────
    else if (a.op == "search") {
        if (a.query.empty()) { wv_done(L"検索ワードを入力してください", false); g_busy=false; return; }
        std::ifstream fin(a.inPath, std::ios::binary);
        if (!fin) { wv_done(L"ファイルが開けません", false); g_busy=false; return; }
        std::vector<uint8_t> container(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>());

        auto be = make_backend(BackendType::RAW);
        if (!be->deserialize(container)) { wv_done(L"解析失敗", false); g_busy=false; return; }
        auto vf = be->read_frame(0);

        std::vector<uint8_t> pat(a.query.begin(), a.query.end());
        int hits = 0;
        for (size_t i=0; i+pat.size()<=vf.data.size(); ++i) {
            if (std::equal(pat.begin(), pat.end(), vf.data.begin()+i)) {
                char buf[64]; snprintf(buf,sizeof(buf),"  offset %zu  len %zu", i, pat.size());
                wv_log(buf, L"l-info");
                ++hits;
            }
        }
        char summ[64]; snprintf(summ,sizeof(summ),
            "[SEARCH] '%s' → %d hit%s", a.query.c_str(), hits, hits==1?"":"s");
        wv_log(summ, hits>0 ? L"l-done" : L"l-warn");
        wv_done(std::wstring(summ,summ+strlen(summ)), hits>0);
    }

    g_busy = false;
}

// ── WebMessage 受信ハンドラ ───────────────────────────────────
static void on_message(const wchar_t* json) {
    // シンプルな JSON パース (production ではRapidJSONなど使用)
    auto get = [&](const std::wstring& key) -> std::wstring {
        auto pos = std::wstring(json).find(L"\"" + key + L"\":");
        if (pos == std::wstring::npos) return {};
        pos += key.size() + 3;
        if (json[pos] == L'"') {
            ++pos;
            std::wstring val;
            while (json[pos] && json[pos] != L'"') {
                if (json[pos]==L'\\' && json[pos+1]==L'\\') { val+=L'\\'; pos+=2; }
                else if (json[pos]==L'\\' && json[pos+1]==L'"') { val+=L'"'; pos+=2; }
                else val += json[pos++];
            }
            return val;
        }
        // boolean/number
        std::wstring val;
        while (json[pos] && json[pos] != L',' && json[pos] != L'}') val += json[pos++];
        return val;
    };

    std::wstring type = get(L"type");

    if (type == L"op") {
        if (g_busy) return;
        g_busy = true;
        OpArgs a;
        a.op       = ws2s(get(L"op"));
        a.inPath   = ws2s(get(L"inPath"));
        a.outPath  = ws2s(get(L"outPath"));
        a.dictPath = ws2s(get(L"dictPath"));
        a.query    = ws2s(get(L"query"));
        a.profile  = std::stoi(get(L"profile").empty() ? L"0" : get(L"profile"));
        a.learning = (get(L"learning") == L"true");
        std::thread(run_op, a).detach();
    }
    else if (type == L"browse") {
        std::wstring mode = get(L"mode");
        std::wstring tid  = get(L"targetId");
        std::wstring path;
        if (mode == L"open")
            path = browse_dialog(g_hwnd, false);
        else if (mode == L"save")
            path = browse_dialog(g_hwnd, true,
                L"CPB Container\0*.raw;*.cpb\0All Files\0*.*\0\0");
        else if (mode == L"dict")
            path = browse_dialog(g_hwnd, false,
                L"Dictionary\0*.dict\0All Files\0*.*\0\0");
        if (!path.empty()) {
            wv_send(L"{\"type\":\"setPath\",\"targetId\":\"" + tid +
                    L"\",\"path\":\"" + json_esc(path) + L"\"}");
        }
    }
}

// ── ウィンドウプロシージャ ────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_SIZE:
        if (g_wvCtrl) {
            RECT rc; GetClientRect(hwnd, &rc);
            g_wvCtrl->put_Bounds(rc);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* m = (MINMAXINFO*)lp;
        m->ptMinTrackSize = {640, 540};
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── WinMain ───────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    // COM
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ユーザーデータ dir
    wchar_t appData[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    g_userDataDir = std::wstring(appData) + L"\\CPB_App";

    // ウィンドウクラス
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"CPB_App";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0,
        L"CPB_App", L"CPB — Cocoa Powder Bottle",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 600,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nShow);

    // WebView2 初期化
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, g_userDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                RECT rc; GetClientRect(g_hwnd, &rc);
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            g_wvCtrl = ctrl;
                            ctrl->get_CoreWebView2(&g_wv);

                            // 設定
                            wil::com_ptr<ICoreWebView2Settings> settings;
                            g_wv->get_Settings(&settings);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);

                            // サイズ
                            RECT rc; GetClientRect(g_hwnd, &rc);
                            ctrl->put_Bounds(rc);
                            ctrl->put_IsVisible(TRUE);

                            // メッセージハンドラ
                            g_wv->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR json = nullptr;
                                        args->get_WebMessageAsJson(&json);
                                        if (json) { on_message(json); CoTaskMemFree(json); }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // HTML を直接ナビゲート
                            g_wv->NavigateToString(
                                std::wstring(UI_HTML, UI_HTML+strlen(UI_HTML)).c_str());

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    // メッセージループ
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
