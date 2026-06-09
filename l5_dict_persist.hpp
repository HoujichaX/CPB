#pragma once
// l5_dict_persist.hpp — L5学習キャッシュの永続化 (Win32対応)
// フォーマット: MAGIC(8) + count(32) + [hash(64)+orig_size(64)+dsl_len(32)+dsl]*N
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>   // memcmp
#include <fstream>
#include "cpb_helpers.hpp"
#include "layer_pipeline.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const uint8_t L5P_MAGIC[8] = {
    0xC5,0xCA,0xC4,0x08, 0x50,0x45,0x52,0x53
};

// ─── 保存 ──────────────────────────────────────────────────────
inline bool l5_cache_save(const std::wstring& wpath)
{
    std::vector<uint8_t> out;
    out.insert(out.end(), L5P_MAGIC, L5P_MAGIC+8);
    auto entries = l5_cache_export();
    w32(out, (uint32_t)entries.size());
    for(auto& e : entries) {
        w64(out, e.hash);
        w64(out, (uint64_t)e.orig_size);
        w32(out, (uint32_t)e.dsl.size());
        out.insert(out.end(), e.dsl.begin(), e.dsl.end());
    }

#ifdef _WIN32
    HANDLE hf = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hf == INVALID_HANDLE_VALUE) return false;
    // 4GB超対応: DWORD単位で分割書き込み
    const uint8_t* ptr = out.data();
    size_t remain = out.size();
    while(remain > 0){
        DWORD chunk = (DWORD)std::min(remain, (size_t)0x7FFFFFFF);
        DWORD written = 0;
        if(!WriteFile(hf, ptr, chunk, &written, nullptr) || written != chunk){
            CloseHandle(hf); return false;
        }
        ptr += chunk; remain -= chunk;
    }
    CloseHandle(hf);
    return true;
#else
    // 非Win32: UTF-8パス前提 (wstring→UTF-8変換なし・ASCII/UTF-8のみ対応)
    std::string path(wpath.begin(), wpath.end());
    std::ofstream f(path, std::ios::binary);
    if(!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return f.good();
#endif
}

// std::string オーバーロード (UTF-8パス)
inline bool l5_cache_save(const std::string& path){
#ifdef _WIN32
    // Win32: UTF-8→UTF-16変換
    int wlen = MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,nullptr,0);
    std::wstring wpath(wlen>0?wlen-1:0,L'\0');
    if(wlen>1) MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,&wpath[0],wlen);
    return l5_cache_save(wpath);
#else
    std::wstring w(path.begin(),path.end());
    return l5_cache_save(w);
#endif
}

// ─── 読み込み ──────────────────────────────────────────────────
inline bool l5_cache_load(const std::wstring& wpath)
{
    std::vector<uint8_t> data;

#ifdef _WIN32
    HANDLE hf = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hf == INVALID_HANDLE_VALUE) return false;
    // 4GB超対応: GetFileSizeEx を使用
    LARGE_INTEGER li{};
    if(!GetFileSizeEx(hf, &li) || li.QuadPart < 12){
        CloseHandle(hf); return false;
    }
    // L5辞書が4GBを超えるケースは現実的にないが念のためガード
    if(li.QuadPart > (LONGLONG)512 * 1024 * 1024){
        CloseHandle(hf); return false; // 512MB超はエラー
    }
    DWORD fsz = (DWORD)li.QuadPart;
    data.resize(fsz);
    DWORD total_read = 0;
    uint8_t* ptr = data.data();
    DWORD remain = fsz;
    while(remain > 0){
        DWORD chunk = std::min(remain, (DWORD)0x7FFFFFFF);
        DWORD rd = 0;
        if(!ReadFile(hf, ptr, chunk, &rd, nullptr) || rd != chunk){
            CloseHandle(hf); return false;
        }
        ptr += rd; remain -= rd; total_read += rd;
    }
    CloseHandle(hf);
    if(total_read != fsz) return false;
#else
    // 非Win32: UTF-8パス前提
    std::string path(wpath.begin(), wpath.end());
    std::ifstream f(path, std::ios::binary);
    if(!f) return false;
    data.assign((std::istreambuf_iterator<char>(f)),
                 std::istreambuf_iterator<char>());
#endif

    if(data.size() < 12) return false;
    if(memcmp(data.data(), L5P_MAGIC, 8) != 0) return false;

    const uint8_t* p   = data.data() + 8;
    const uint8_t* end = data.data() + data.size();
    uint32_t n = r32(p);

    // 部分破損ポリシー:
    //   途中でデータが壊れていたら読めた分だけ import して true を返す。
    //   「一部でも使える方が全損よりマシ」という実用優先の設計。
    //   完全性チェックが必要な場合は戻り値に partial フラグを追加する。
    std::vector<DslCacheEntry> entries;
    entries.reserve(std::min(n, (uint32_t)65536)); // 異常に大きいnをガード
    bool partial = false;
    for(uint32_t i = 0; i < n; ++i) {
        if(p + 20 > end){ partial = true; break; }  // hash(8)+orig(8)+len(4)
        DslCacheEntry e;
        e.hash      = r64(p);
        e.orig_size = (size_t)r64(p);
        uint32_t dsz = r32(p);
        if(dsz > 64 * 1024 * 1024){ partial = true; break; } // 64MB超は異常値
        if(p + dsz > end){ partial = true; break; }
        e.dsl.assign(p, p + dsz);
        p += dsz;
        entries.push_back(std::move(e));
    }
    (void)partial; // 現在は部分ロードも成功扱い

    l5_cache_import(entries);
    return !entries.empty();
}

// std::string オーバーロード (UTF-8パス)
inline bool l5_cache_load(const std::string& path){
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,nullptr,0);
    std::wstring wpath(wlen>0?wlen-1:0,L'\0');
    if(wlen>1) MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,&wpath[0],wlen);
    return l5_cache_load(wpath);
#else
    std::wstring w(path.begin(),path.end());
    return l5_cache_load(w);
#endif
}
