#pragma once
#include <vector>
#include <cmath>
#include <string>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <optional>

// ============================================================
// CPB Level 4 — 多次元データマッピング (拡張可能設計)
//
// 現在の実装次元:
//   D1 X: ピクセル横     (0 .. width-1)
//   D2 Y: ピクセル縦     (0 .. height-1)
//   D3 Z: 時間/フレーム  (0 .. frames-1)
//   D5 W: 音声サンプル   (0 .. audio_samples-1)
//
// 将来追加可能な次元 (DimID で管理):
//   D4 C: 色チャンネル   Y=0/Cb=1/Cr=2  (FFV1 4:4:4時)
//   D6 K: 音声チャンネル L=0/R=1
//   D7 B: ビット平面     0=LSB .. 7=MSB
//   D8..: 将来拡張用
//
// 設計方針:
//   - CellAddress は現在の4フィールド固定を維持 (後方互換)
//   - FourDSpace に DimExtension を追加することで次元を増やせる
//   - 既存の map_payload/unmap_payload は変更なし
//   - 次元追加時は add_dimension() を呼ぶだけ
// ============================================================

// ── 次元ID ──
enum class DimID : uint8_t {
    X = 0x01,  // ピクセル横 (必須)
    Y = 0x02,  // ピクセル縦 (必須)
    Z = 0x03,  // フレーム   (必須)
    W = 0x04,  // 音声サンプル
    C = 0x05,  // 色チャンネル (D4: Y/Cb/Cr)
    K = 0x06,  // 音声チャンネル (D6: L/R)
    B = 0x07,  // ビット平面 (D7: 0=LSB)
    // 0x08 以降は将来予約
};

inline const char* dim_name(DimID d) {
    switch(d){
    case DimID::X: return "X(pixel_x)";
    case DimID::Y: return "Y(pixel_y)";
    case DimID::Z: return "Z(frame)";
    case DimID::W: return "W(audio)";
    case DimID::C: return "C(color_ch)";
    case DimID::K: return "K(audio_ch)";
    case DimID::B: return "B(bit_plane)";
    default:       return "?(unknown)";
    }
}

// ── 拡張次元定義 ──
// FourDSpace に追加する次元。値域 [0, size-1] を持つ。
struct DimExtension {
    DimID    id;
    uint32_t size;       // この次元の値域
    std::string note;    // 人間向け説明
};

// ── セルアドレス (後方互換: 4フィールド固定) ──
struct CellAddress {
    uint16_t x = 0;        // D1: 横ピクセル
    uint16_t y = 0;        // D2: 縦ピクセル
    uint32_t z = 0;        // D3: フレーム番号  (UINT32_MAX = 音声セル)
    uint32_t w = 0;        // D5: 音声サンプル位置 (UINT32_MAX = 映像セル)

    // 拡張次元の値 (optional: 次元が有効な場合のみ設定)
    // DimID → value のマップ。メモリ効率より拡張性を優先。
    // 追加次元が不要なら空のまま → 既存コードに影響なし
    std::unordered_map<uint8_t, uint32_t> ext;

    // 拡張次元の値を取得 (次元未設定なら 0)
    uint32_t get(DimID d) const {
        auto it = ext.find((uint8_t)d);
        return it != ext.end() ? it->second : 0;
    }
    void set(DimID d, uint32_t v) { ext[(uint8_t)d] = v; }
    bool has(DimID d) const { return ext.count((uint8_t)d) > 0; }
};

// ── 多次元空間の仕様 ──
struct FourDSpace {
    // ── 基本4次元 (必須) ──
    uint16_t width         = 1920;
    uint16_t height        = 1080;
    uint32_t frames        = 0;
    uint32_t audio_samples = 0;
    uint32_t audio_rate    = 48000;
    uint8_t  channels      = 2;

    // ── 拡張次元 (任意: 後から追加可能) ──
    std::vector<DimExtension> dims;

    // 拡張次元を追加
    FourDSpace& add_dimension(DimID id, uint32_t size,
                               const std::string& note = "")
    {
        // 既存なら上書き
        for(auto& d : dims) if(d.id == id) { d.size=size; d.note=note; return *this; }
        dims.push_back({id, size, note});
        return *this;
    }

    // 拡張次元を取得
    std::optional<DimExtension> get_dim(DimID id) const {
        for(auto& d : dims) if(d.id == id) return d;
        return std::nullopt;
    }
    bool has_dim(DimID id) const {
        for(auto& d : dims) if(d.id == id) return true;
        return false;
    }

    // 基本セル数 (X×Y×Z + W)
    uint64_t video_cells() const {
        return (uint64_t)width * height * frames;
    }
    uint64_t base_cells() const {
        return video_cells() + (uint64_t)audio_samples;
    }

    // 拡張次元を含む総セル数
    // 拡張次元は基本セルに対して乗算的に作用する
    // (例: D4=3 なら全セルが3倍になる)
    uint64_t total_cells() const {
        uint64_t n = base_cells();
        for(auto& d : dims) n *= d.size;
        return n;
    }

    // 次元の一覧を表示
    std::string describe() const {
        std::string s = "FourDSpace[\n";
        s += "  D1 X=" + std::to_string(width)  + "\n";
        s += "  D2 Y=" + std::to_string(height) + "\n";
        s += "  D3 Z=" + std::to_string(frames) + "\n";
        s += "  D5 W=" + std::to_string(audio_samples) + "\n";
        for(auto& d : dims)
            s += "  " + std::string(dim_name(d.id))
               + "=" + std::to_string(d.size)
               + " (" + d.note + ")\n";
        uint64_t tc = total_cells();
        s += "  total=" + std::to_string(tc)
           + " (2^" + std::to_string((int)(log2((double)tc))) + ")\n";
        s += "]";
        return s;
    }
};

// マッピング結果
struct FourDMapping {
    FourDSpace                space;
    uint64_t                  seed;
    std::vector<CellAddress>  addresses;
    uint8_t                   bits_per_cell = 1;
};

// マッピング統計
struct MappingStats {
    uint64_t total_cells;
    uint64_t used_cells;
    double   density;
    double   video_usage_pct;
    double   audio_usage_pct;
    uint64_t key_space;
    size_t   active_dims;   // 有効な次元数 (基本4 + 拡張)
};

// ============================================================
// フレームリーダー / ライター
// ============================================================
using PixelReader = std::function<uint8_t(uint32_t frame, uint16_t x, uint16_t y)>;
using PixelWriter = std::function<void(uint32_t frame, uint16_t x, uint16_t y, uint8_t val)>;
using AudioReader = std::function<uint8_t(uint32_t sample)>;
using AudioWriter = std::function<void(uint32_t sample, uint8_t val)>;

// ============================================================
// API (既存互換)
// ============================================================

// エンコード
FourDMapping map_payload(
    const std::vector<uint8_t>& payload,
    const FourDSpace& space,
    uint64_t seed,
    uint8_t bits_per_cell = 1);

// デコード (テスト用)
std::vector<uint8_t> unmap_payload(const FourDMapping& mapping);

// デコード (本番: 実フレームから読み取り)
std::vector<uint8_t> unmap_payload_from_frames(
    const FourDMapping& mapping,
    const PixelReader& pixel_reader,
    const AudioReader& audio_reader = nullptr);

// エンコード (本番: 実フレームに書き込み)
void map_payload_to_frames(
    const FourDMapping& mapping,
    const std::vector<uint8_t>& payload,
    const PixelWriter& pixel_writer,
    const AudioWriter& audio_writer = nullptr);

// 統計
MappingStats calc_stats(const FourDMapping& mapping);

// シリアライズ
std::vector<uint8_t> serialize_mapping(const FourDMapping& m);
FourDMapping deserialize_mapping(const std::vector<uint8_t>& buf);

// ユーティリティ
struct PixelPos { uint16_t x, y; uint32_t frame; };
PixelPos cell_to_pixel(const CellAddress& addr);

struct AudioPos { uint32_t sample; uint8_t channel; };
AudioPos cell_to_audio(const CellAddress& addr, uint8_t channels);

std::string keyspace_description(const FourDSpace& space);

// ============================================================
// 便利ファクトリ関数
// ============================================================

// 標準4D空間
inline FourDSpace make_space_4d(
    uint16_t w, uint16_t h, uint32_t frames, uint32_t audio)
{
    FourDSpace s;
    s.width=w; s.height=h; s.frames=frames; s.audio_samples=audio;
    return s;
}

// 7D空間 (色差 + 音声チャンネル + ビット平面)
// FFV1 4:4:4 で録画した場合に使用
inline FourDSpace make_space_7d(
    uint16_t w, uint16_t h, uint32_t frames, uint32_t audio,
    uint8_t bit_planes = 3)
{
    FourDSpace s;
    s.width=w; s.height=h; s.frames=frames; s.audio_samples=audio;
    s.add_dimension(DimID::C, 3,  "Y/Cb/Cr color channels (FFV1 4:4:4)");
    s.add_dimension(DimID::K, 2,  "stereo audio L/R channels");
    s.add_dimension(DimID::B, bit_planes,
                    "LSB bit planes (0=bit0 .. N=bitN)");
    return s;
}

// カスタム空間 (好きな次元を追加)
inline FourDSpace make_space_custom(
    uint16_t w, uint16_t h, uint32_t frames, uint32_t audio)
{
    return make_space_4d(w, h, frames, audio);
    // → .add_dimension(DimID::C, 3) などで後から追加
}
