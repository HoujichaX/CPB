#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// cpb_config.hpp — CPB モジュール選択 & パラメータ設定
//
// 使い方:
//   1. CPBConfig を作って使用モジュールと定数を設定
//   2. pack/unpack に渡すと各モジュールがそれを参照
//   3. ヘッダーに config_id (1B) を記録 → 復元時に自動再現
// ============================================================

// ── モジュールフラグ (OR で組み合わせ) ──
enum class CPBModule : uint8_t {
    NONE        = 0x00,
    PROTECTION  = 0x01, // Layer 1: RS / XOR / Fountain / SemanticNoise
    COMPRESSION = 0x02, // Layer 2: Zstd/LZ4/Delta/RLE + 基本DSL
    GENRE_DSL   = 0x04, // Layer 3: ジャンル別DSL (構造的圧縮)
    FOURD_MAP   = 0x08, // Layer 4: 4Dデータマッピング (ステガノ強化)
    GEN_DICT    = 0x10, // Layer 5: ジャンル別辞書生成 (生成復元)
    SEARCH      = 0x20, // Base:    FIDX外部検索
};
inline CPBModule operator|(CPBModule a, CPBModule b){
    return (CPBModule)((uint8_t)a|(uint8_t)b);
}
inline bool has_module(CPBModule flags, CPBModule m){
    return ((uint8_t)flags & (uint8_t)m) != 0;
}

// ── RSプリセット ──
struct RSParams {
    size_t dsl_block;    // DSLブロックサイズ
    size_t dsl_parity;   // DSLパリティ
    size_t blob_block;   // Blobブロックサイズ
    size_t blob_parity;  // Blobパリティ

    // 最大訂正バイト数
    size_t dsl_max_correct()  const { return dsl_parity  / 2; }
    size_t blob_max_correct() const { return blob_parity / 2; }

    static RSParams standard() {
        // 現行デフォルト: DSL高冗長, Blob標準
        return {127, 128, 223, 32};
    }
    static RSParams max_protection() {
        // 最高保護: 両方最大パリティ
        return {1, 254, 127, 128};
    }
    static RSParams steganographic() {
        // ステガノ向け: DSL高保護, Blob最高保護
        return {127, 128, 1, 254};
    }
    static RSParams lightweight() {
        // 軽量: パリティ最小 (高速・低耐性)
        return {223, 32, 223, 32};
    }
    static RSParams no_rs() {
        // RSなし (パススルー) ※開発/テスト用
        return {223, 0, 223, 0};
    }
};


// ── 次元設定 (L4 多次元シャッフル) ─────────────────────────
struct DimConfig {
    // 映像系次元
    bool use_z   = true;  // Z: フレーム軸      (基本4D)
    bool use_y   = true;  // Y: 垂直位置         (基本4D)
    bool use_x   = true;  // X: 水平位置         (基本4D)
    bool use_w   = true;  // W: 音声サンプル位置 (基本4D)
    bool use_c   = false; // C: 色チャンネル
    bool use_b   = false; // B: ビット平面
    bool use_bx  = false; // BX: ブロック位置X
    bool use_by  = false; // BY: ブロック位置Y
    bool use_fv  = false; // Fv: 周波数成分(DCT)
    bool use_th  = false; // θ: エッジ方向
    bool use_tx  = false; // TX: テクスチャ分類
    // 音声系次元
    bool use_k   = false; // K: 音声チャンネル
    bool use_fa  = false; // Fa: 周波数帯域
    bool use_t   = false; // T: 時間窓
    bool use_e   = false; // E: エネルギー量子化
    bool use_ba  = false; // Ba: 音声ビット平面

    int active_count() const {
        return (int)(use_z+use_y+use_x+use_w+use_c+use_b+
                     use_bx+use_by+use_fv+use_th+use_tx+
                     use_k+use_fa+use_t+use_e+use_ba);
    }
    // dim_flags: 16bit (bit0=Z ... bit15=Ba)
    uint16_t to_flags() const {
        return (uint16_t)(
            (use_z?1:0)|(use_y?2:0)|(use_x?4:0)|(use_w?8:0)|
            (use_c?16:0)|(use_b?32:0)|(use_bx?64:0)|(use_by?128:0)|
            (use_fv?256:0)|(use_th?512:0)|(use_tx?1024:0)|
            (use_k?2048:0)|(use_fa?4096:0)|(use_t?8192:0)|
            (use_e?16384:0)|(use_ba?32768:0));
    }
    static DimConfig from_flags(uint16_t f){
        DimConfig d;
        d.use_z=(f&1)!=0;   d.use_y=(f&2)!=0;    d.use_x=(f&4)!=0;
        d.use_w=(f&8)!=0;   d.use_c=(f&16)!=0;   d.use_b=(f&32)!=0;
        d.use_bx=(f&64)!=0; d.use_by=(f&128)!=0; d.use_fv=(f&256)!=0;
        d.use_th=(f&512)!=0;d.use_tx=(f&1024)!=0;d.use_k=(f&2048)!=0;
        d.use_fa=(f&4096)!=0;d.use_t=(f&8192)!=0;d.use_e=(f&16384)!=0;
        d.use_ba=(f&32768)!=0;
        return d;
    }
    // プリセット
    static DimConfig dim4()  { return DimConfig{}; } // Z+Y+X+W
    static DimConfig dim8()  { auto d=dim4();  d.use_c=d.use_b=d.use_k=d.use_fa=true; return d; }
    static DimConfig dim12() { auto d=dim8();  d.use_bx=d.use_by=d.use_fv=d.use_t=true; return d; }
    static DimConfig dim16() { auto d=dim12(); d.use_th=d.use_tx=d.use_e=d.use_ba=true; return d; }
    static DimConfig all()   { DimConfig d; d.use_c=d.use_b=d.use_bx=d.use_by=d.use_fv=d.use_th=d.use_tx=d.use_k=d.use_fa=d.use_t=d.use_e=d.use_ba=true; return d; }
};

// ── 設定プリセット名 ──
enum class CPBPreset : uint8_t {
    CUSTOM        = 0x00, // 個別指定
    ARCHIVE       = 0x01, // 公文書アーカイブ (Base + Search + 4D)
    GAME_ASSET    = 0x02, // ゲームアセット (Base + Compress + Genre)
    SURVEILLANCE  = 0x03, // 監視映像 (Base + Genre + 4D)
    MAX_STEGANО   = 0x04, // 最強ステガノ (Base + 4D + NONE圧縮)
    FULL          = 0x05, // 全部入り
};

// ── CPB 設定本体 ──
struct CPBConfig {
    CPBPreset  preset  = CPBPreset::CUSTOM;
    CPBModule  modules = CPBModule::NONE;
    RSParams   rs      = RSParams::standard();

    // Layer 1: 圧縮
    uint8_t compress_method = 0xFF; // AUTO=0xFF
    uint8_t compress_level  = 3;

    // Layer 3: 4Dマッピング
    uint64_t fourd_seed         = 0;

    // 学習モード設定
    // false (default): L5 lookup-only (高速)
    //                  L5 → L3 → L2 → L1 の順序
    //                  辞書ヒットでL3をスキップ、ミスはL3が処理
    // true:            L5_LEARN (低速、辞書を育てる)
    //                  L3 → L5_LEARN → L2 → L1 の順序
    //                  L3の出力をDictRegistryにキャッシュ、DictEvolutionに観測提出
    bool     learning           = false;
    std::string l3_dict_path;
    DimConfig  dims;               // L4 次元設定   // L3フレーズ辞書 (.cpbdict)
    double   learn_threshold    = 0.90;  // 辞書ヒットと見なす類似度閾値

    // 7D空間設定 (false = 標準4D)
    bool     use_color_dim      = false;  // D4: Y/Cb/Cr × 3
    bool     use_stereo_dim     = false;  // D6: L/R × 2
    uint8_t  bit_plane_depth    = 0;      // D7: 0=無効, 1〜7=使用するbit平面数
    uint8_t  fourd_bits_per_cell= 1;

    // ── プリセットファクトリ ──
    static CPBConfig archive() {
        CPBConfig c;
        c.preset  = CPBPreset::ARCHIVE;
        c.modules = CPBModule::SEARCH | CPBModule::FOURD_MAP | CPBModule::PROTECTION;
        c.rs      = RSParams::max_protection();
        c.compress_method = 0x00;
        return c;
    }
    static CPBConfig game_asset() {
        CPBConfig c;
        c.preset  = CPBPreset::GAME_ASSET;
        c.modules = CPBModule::COMPRESSION | CPBModule::GENRE_DSL | CPBModule::PROTECTION;
        c.rs      = RSParams::standard();
        c.compress_method = 0x02;
        return c;
    }
    static CPBConfig surveillance() {
        CPBConfig c;
        c.preset  = CPBPreset::SURVEILLANCE;
        c.modules = CPBModule::GENRE_DSL | CPBModule::FOURD_MAP
                  | CPBModule::SEARCH    | CPBModule::PROTECTION;
        c.rs      = RSParams::standard();
        c.compress_method = 0xFF;
        return c;
    }
    static CPBConfig max_stegano(uint64_t seed) {
        CPBConfig c;
        c.preset  = CPBPreset::MAX_STEGANО;
        c.modules = CPBModule::FOURD_MAP | CPBModule::PROTECTION;
        c.rs      = RSParams::steganographic();
        c.compress_method = 0x00;
        c.fourd_seed      = seed;
        return c;
    }
    // Layer5: AI学習データ / 典型的なアセット向け
    static CPBConfig ai_dataset(uint64_t seed = 0) {
        CPBConfig c;
        c.preset  = CPBPreset::CUSTOM;
        c.modules = CPBModule::COMPRESSION | CPBModule::GENRE_DSL
                  | CPBModule::GEN_DICT    | CPBModule::PROTECTION;
        c.rs      = RSParams::standard();
        c.compress_method = 0xFF; // AUTO
        c.fourd_seed      = seed;
        return c;
    }
    static CPBConfig full(uint64_t fourd_seed = 0) {
        CPBConfig c;
        c.preset  = CPBPreset::FULL;
        c.modules = CPBModule::PROTECTION
                  | CPBModule::COMPRESSION
                  | CPBModule::GENRE_DSL
                  | CPBModule::FOURD_MAP
                  | CPBModule::GEN_DICT
                  | CPBModule::SEARCH;
        c.rs      = RSParams::standard();
        c.compress_method = 0xFF;
        c.fourd_seed      = fourd_seed;
        return c;
    }

    // ── シリアライズ (TLV形式: v1=14バイト, 将来拡張可) ──
    // フォーマット: [ver:1][len:1][fields:N]
    //   ver=1: preset(1)+modules(1)+dsl_block(1)+dsl_parity(1)+
    //          blob_block(1)+blob_parity(1)+compress_method(1)+
    //          compress_level(1)+fourd_bits(1)+reserved(3) = 12フィールド
    static constexpr uint8_t SERIAL_VERSION = 1;
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> fields = {
            (uint8_t)preset, (uint8_t)modules,
            (uint8_t)rs.dsl_block, (uint8_t)rs.dsl_parity,
            (uint8_t)rs.blob_block, (uint8_t)rs.blob_parity,
            compress_method, compress_level,
            fourd_bits_per_cell, 0, 0, 0
        };
        std::vector<uint8_t> b;
        b.push_back(SERIAL_VERSION);
        b.push_back((uint8_t)fields.size());
        b.insert(b.end(), fields.begin(), fields.end());
        return b; // 14バイト (v1)
    }
    static CPBConfig deserialize(const std::vector<uint8_t>& b) {
        CPBConfig c;
        if(b.size() < 2) return c;
        uint8_t ver = b[0];
        uint8_t len = b[1];
        if(b.size() < 2u + len) return c;
        // v1 フィールド (後方互換: len>=12 なら読む)
        if(ver >= 1 && len >= 12) {
            c.preset              = (CPBPreset)b[2];
            c.modules             = (CPBModule)b[3];
            c.rs.dsl_block        = b[4];
            c.rs.dsl_parity       = b[5];
            c.rs.blob_block       = b[6];
            c.rs.blob_parity      = b[7];
            c.compress_method     = b[8];
            c.compress_level      = b[9];
            c.fourd_bits_per_cell = b[10];
        }
        // v2以降: ここに追加フィールドを追加
        return c;
    }

    // ── 説明文 ──
    std::string describe() const {
        std::string s = "CPBConfig[";
        switch(preset){
        case CPBPreset::ARCHIVE:      s+="ARCHIVE";      break;
        case CPBPreset::GAME_ASSET:   s+="GAME_ASSET";   break;
        case CPBPreset::SURVEILLANCE: s+="SURVEILLANCE"; break;
        case CPBPreset::MAX_STEGANО:  s+="MAX_STEGANO";  break;
        case CPBPreset::FULL:         s+="FULL";         break;
        default:                      s+="CUSTOM";       break;
        }
        s += "] modules={";
        if(has_module(modules, CPBModule::PROTECTION))  s+="Protection,";
        if(has_module(modules, CPBModule::COMPRESSION)) s+="Compress,";
        if(has_module(modules, CPBModule::GENRE_DSL))   s+="GenreDSL,";
        if(has_module(modules, CPBModule::FOURD_MAP))   s+="4D,";
        if(has_module(modules, CPBModule::GEN_DICT))    s+="GenDict,";
        if(has_module(modules, CPBModule::SEARCH))      s+="Search,";
        s += "} RS(DSL:"+std::to_string(rs.dsl_block)+"/"
            +std::to_string(rs.dsl_parity)
            +" Blob:"+std::to_string(rs.blob_block)+"/"
            +std::to_string(rs.blob_parity)+")";
        return s;
    }
};

// ── 後方互換: 既存コードが使うグローバル定数を config から設定 ──
// container.hpp の RS_BLOB_BLOCK 等をこちらで上書きする場合に使用
inline void apply_rs_params(const RSParams& p,
                             size_t& dsl_block,  size_t& dsl_parity,
                             size_t& blob_block, size_t& blob_parity) {
    dsl_block   = p.dsl_block;
    dsl_parity  = p.dsl_parity;
    blob_block  = p.blob_block;
    blob_parity = p.blob_parity;
}
