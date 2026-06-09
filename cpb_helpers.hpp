#pragma once
// cpb_helpers.hpp — 共通シリアライズヘルパー (inline定義)
// 複数.cppをincludeするテスト環境でも衝突しない
#include <vector>
#include <cstdint>
#include <string>

inline void w8 (std::vector<uint8_t>& o, uint8_t  v){ o.push_back(v); }
static inline void w16(std::vector<uint8_t>& o, uint16_t v){
    o.push_back((uint8_t)(v)); o.push_back((uint8_t)(v>>8)); }
static inline void w32(std::vector<uint8_t>& o, uint32_t v){
    for(int i=0;i<4;++i) o.push_back((uint8_t)((v>>(i*8))&0xFF)); }
static inline void w64(std::vector<uint8_t>& o, uint64_t v){
    for(int i=0;i<8;++i) o.push_back((uint8_t)((v>>(i*8))&0xFF)); }

inline uint8_t  r8 (const uint8_t*& p){ return *p++; }
static inline uint16_t r16(const uint8_t*& p){
    uint16_t v=(uint16_t)p[0]|((uint16_t)p[1]<<8); p+=2; return v; }
static inline uint32_t r32(const uint8_t*& p){
    uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t)(*p++)<<(i*8); return v; }
static inline uint64_t r64(const uint8_t*& p){
    uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t)(*p++)<<(i*8); return v; }

inline void wstr(std::vector<uint8_t>& o, const std::string& s){
    w32(o,(uint32_t)s.size());
    o.insert(o.end(),s.begin(),s.end()); }
inline std::string rstr(const uint8_t*& p){
    uint32_t n=r32(p);
    std::string s((const char*)p,n); p+=n; return s; }
