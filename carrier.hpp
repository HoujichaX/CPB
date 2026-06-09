#pragma once
// carrier.hpp — CPBコンテナを各種ファイル形式に包む/取り出す
// 書き込み: wrap_container(data, format) → バイト列
// 読み出し: unwrap_container(bytes)       → CPBバイト列

#include <vector>
#include <cstdint>
#include <string>

enum class CarrierFormat : uint8_t {
    CPB  = 0,  // .cpb  ネイティブ (デフォルト)
    RAW  = 1,  // .raw  旧形式互換
    ZIP  = 2,  // .zip  ZIP偽装 (囮ファイル入り)
    MP4  = 3,  // .mp4  動画偽装
    PDF  = 4,  // .pdf  文書偽装
    PNG  = 5,  // .png  画像偽装
};

inline const wchar_t* carrier_ext(CarrierFormat f){
    switch(f){
    case CarrierFormat::ZIP: return L".zip";
    case CarrierFormat::MP4: return L".mp4";
    case CarrierFormat::PDF: return L".pdf";
    case CarrierFormat::PNG: return L".png";
    case CarrierFormat::RAW: return L".raw";
    default:                 return L".cpb";
    }
}

// CPBデータをキャリアで包む
std::vector<uint8_t> carrier_wrap(
    const std::vector<uint8_t>& cpb_data,
    CarrierFormat fmt,
    const std::string& decoy_name = "readme.txt");

// キャリアからCPBデータを取り出す (形式自動判定)
std::vector<uint8_t> carrier_unwrap(const std::vector<uint8_t>& file_data);

// ファイルがCPBキャリアかどうか判定
bool is_cpb_carrier(const std::vector<uint8_t>& file_data);
