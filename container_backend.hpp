#pragma once
#include <vector>
#include <cstring>
#include <string>
#include <cstdint>
#include <memory>
#include <functional>

// ============================================================
// CPB Container Backend — コンテナ形式の抽象化
//
// 「FFV1固定リスク」を排除し、将来のコーデックに対応する。
//
// IContainerBackend
//   ├─ FFV1MKVBackend      (デフォルト: 可逆・長期保存)
//   ├─ H264MP4Backend      (ステガノ: 人間用・高互換)
//   ├─ PNGSequenceBackend  (開発用: デバッグ・検証)
//   └─ RawBinaryBackend    (最軽量: AI通信用)
//
// AI→AI通信では RawBinaryBackend が最適。
// 人間→人間配布では H264MP4Backend がベスト。
// 長期アーカイブでは FFV1MKVBackend が最強。
// ============================================================

// ── フレームデータ ──
struct VideoFrame {
    uint32_t             frame_id = 0;
    std::vector<uint8_t> data;      // ピクセルまたはバイナリデータ
    uint32_t             width  = 0;
    uint32_t             height = 0;
    double               pts_sec= 0.0; // タイムスタンプ
};

// ── コンテナ情報 ──
struct ContainerInfo {
    std::string format_name;     // "FFV1/MKV", "H264/MP4", ...
    bool        lossless  = true;
    bool        seekable  = true;
    double      fps       = 30.0;
    uint32_t    width     = 1920;
    uint32_t    height    = 1080;
    uint32_t    frame_count = 0;
    std::string description;
};

// ── コンテナバックエンドインターフェース ──
class IContainerBackend {
public:
    virtual ~IContainerBackend() = default;

    // コンテナ情報
    virtual ContainerInfo info() const = 0;

    // フレームを書き込む
    virtual bool write_frame(const VideoFrame& frame) = 0;

    // フレームを読み込む
    virtual VideoFrame read_frame(uint32_t frame_id) = 0;

    // 全フレーム数
    virtual uint32_t frame_count() const = 0;

    // シーク
    virtual bool seek(uint32_t frame_id) = 0;

    // バッファをシリアライズ (ファイル書き出し用)
    virtual std::vector<uint8_t> serialize() const = 0;

    // バッファからデシリアライズ (読み込み用)
    virtual bool deserialize(const std::vector<uint8_t>& buf) = 0;

    // ロスレスか
    virtual bool is_lossless() const = 0;
};

// ============================================================
// RawBinaryBackend — 最軽量 (AI通信・テスト用)
// フレームをそのままバイト列で格納。
// オーバーヘッド最小、AI間通信に最適。
// ============================================================
class RawBinaryBackend : public IContainerBackend {
public:
    explicit RawBinaryBackend(double fps=30.0) : fps_(fps) {}

    ContainerInfo info() const override {
        ContainerInfo i;
        i.format_name="RawBinary";
        i.lossless=true; i.seekable=true;
        i.fps=fps_; i.frame_count=(uint32_t)frames_.size();
        i.description="Zero-overhead binary frames. AI-to-AI optimized.";
        return i;
    }
    bool write_frame(const VideoFrame& f) override {
        frames_.push_back(f); return true;
    }
    VideoFrame read_frame(uint32_t id) override {
        if(id>=frames_.size()) return {};
        return frames_[id];
    }
    uint32_t frame_count() const override { return (uint32_t)frames_.size(); }
    bool seek(uint32_t) override { return true; }
    bool is_lossless() const override { return true; }

    // フォーマット: [magic:4][fps:8][frame_count:4]
    //               ([frame_id:4][data_size:4][data:N])*
    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> b;
        auto w32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
        auto w64=[&](uint64_t v){for(int i=0;i<8;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
        for(char c:{'C','P','B','R'}) b.push_back((uint8_t)c); // magic
        uint64_t fps_bits; memcpy(&fps_bits,&fps_,8);
        w64(fps_bits);
        w32((uint32_t)frames_.size());
        for(auto& f:frames_){
            w32(f.frame_id);
            w32((uint32_t)f.data.size());
            b.insert(b.end(),f.data.begin(),f.data.end());
        }
        return b;
    }
    bool deserialize(const std::vector<uint8_t>& buf) override {
        if(buf.size()<16) return false;
        const uint8_t* p=buf.data();
        if(memcmp(p,"CPBR",4)!=0) return false; p+=4;
        uint64_t fps_bits=0; for(int i=0;i<8;++i) fps_bits|=(uint64_t)(*p++)<<(i*8);
        memcpy(&fps_,&fps_bits,8);
        uint32_t n=0; for(int i=0;i<4;++i) n|=(uint32_t)(*p++)<<(i*8);
        frames_.clear();
        for(uint32_t i=0;i<n;++i){
            VideoFrame f;
            f.frame_id=0; for(int j=0;j<4;++j) f.frame_id|=(uint32_t)(*p++)<<(j*8);
            uint32_t sz=0; for(int j=0;j<4;++j) sz|=(uint32_t)(*p++)<<(j*8);
            f.data.assign(p,p+sz); p+=sz;
            f.pts_sec=f.frame_id/fps_;
            frames_.push_back(std::move(f));
        }
        return true;
    }
private:
    double fps_;
    std::vector<VideoFrame> frames_;
};

// ============================================================
// PNGSequenceBackend — 開発・デバッグ用
// フレームをPNGライクな独自ヘッダー付きで格納。
// 人間が中身を確認しやすい。
// ============================================================
class PNGSequenceBackend : public IContainerBackend {
public:
    ContainerInfo info() const override {
        ContainerInfo i;
        i.format_name="PNGSequence";
        i.lossless=true; i.seekable=true;
        i.fps=1.0; i.frame_count=(uint32_t)frames_.size();
        i.description="PNG-like sequence. Debug/development use.";
        return i;
    }
    bool write_frame(const VideoFrame& f) override {
        frames_.push_back(f); return true;
    }
    VideoFrame read_frame(uint32_t id) override {
        if(id>=frames_.size()) return {};
        return frames_[id];
    }
    uint32_t frame_count() const override { return (uint32_t)frames_.size(); }
    bool seek(uint32_t) override { return true; }
    bool is_lossless() const override { return true; }

    // フォーマット: [magic:8 "CPBPNGSEQ"]
    //               ([header:frame_id4+size4+checksum4][data:N])*
    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> b;
        const char* magic="CPBPNGSEQ"; // 9 bytes
        b.insert(b.end(),magic,magic+9);
        auto w32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
        w32((uint32_t)frames_.size());
        for(auto& f:frames_){
            w32(f.frame_id); w32((uint32_t)f.data.size());
            // 簡易チェックサム
            uint32_t cs=0; for(auto x:f.data) cs+=x;
            w32(cs);
            b.insert(b.end(),f.data.begin(),f.data.end());
        }
        return b;
    }
    bool deserialize(const std::vector<uint8_t>& buf) override {
        if(buf.size()<13) return false;
        if(memcmp(buf.data(),"CPBPNGSEQ",9)!=0) return false;
        const uint8_t* p=buf.data()+9;
        const uint8_t* end=buf.data()+buf.size();
        uint32_t n=0; for(int i=0;i<4;++i) n|=(uint32_t)(*p++)<<(i*8);
        frames_.clear();
        for(uint32_t i=0;i<n;++i){
            if(p+12>end) return false; // header bounds check
            VideoFrame f;
            f.frame_id=0; for(int j=0;j<4;++j) f.frame_id|=(uint32_t)(*p++)<<(j*8);
            uint32_t sz=0; for(int j=0;j<4;++j) sz|=(uint32_t)(*p++)<<(j*8);
            uint32_t cs=0; for(int j=0;j<4;++j) cs|=(uint32_t)(*p++)<<(j*8);
            if(p+sz>end) return false; // data bounds check
            f.data.assign(p,p+sz); p+=sz;
            // チェックサム検証
            uint32_t actual=0; for(auto x:f.data) actual+=x;
            if(actual!=cs) return false; // 破損検出
            frames_.push_back(std::move(f));
        }
        return true;
    }
private:
    std::vector<VideoFrame> frames_;
};

// ============================================================
// FFV1MKVBackend — 本番用スタブ
// 実際のFFmpegバインディングは将来実装。
// インターフェースだけ定義してテスト可能にする。
// ============================================================
class FFV1MKVBackend : public IContainerBackend {
public:
    ContainerInfo info() const override {
        ContainerInfo i;
        i.format_name="FFV1/MKV";
        i.lossless=true; i.seekable=true; i.fps=30.0;
        i.frame_count=(uint32_t)frames_.size();
        i.description="Lossless FFV1 in Matroska. Long-term archive standard.";
        return i;
    }
    bool write_frame(const VideoFrame& f) override {
        frames_.push_back(f); return true;
    }
    VideoFrame read_frame(uint32_t id) override {
        if(id>=frames_.size()) return {};
        return frames_[id];
    }
    uint32_t frame_count() const override { return (uint32_t)frames_.size(); }
    bool seek(uint32_t) override { return true; }
    bool is_lossless() const override { return true; }
    std::vector<uint8_t> serialize() const override {
#ifdef CPB_REAL_FFMPEG
        // S3: 実FFmpegバックエンド (ffmpeg_backend.hpp)
        return ffmpeg_serialize_ffv1(frames_, 30.0);
#else
        // スタブ: RawBinaryと同じフォーマット
        RawBinaryBackend rb; for(auto& f:frames_) rb.write_frame(f);
        auto buf=rb.serialize(); buf[3]='F';
        return buf;
#endif
    }
    bool deserialize(const std::vector<uint8_t>& buf) override {
#ifdef CPB_REAL_FFMPEG
        return ffmpeg_deserialize_ffv1(buf, frames_);
#else
        if(buf.size()<4) return false;
        auto copy=buf; copy[3]='R';
        RawBinaryBackend rb; if(!rb.deserialize(copy)) return false;
        frames_.clear();
        for(uint32_t i=0;i<rb.frame_count();++i) frames_.push_back(rb.read_frame(i));
        return true;
#endif
    }
private:
    std::vector<VideoFrame> frames_;
};

// ============================================================
// H264MP4Backend — ステガノ・配布用スタブ
// MP4コンテナにデータを埋め込む。
// 人間には普通の動画に見える。
// ============================================================
class H264MP4Backend : public IContainerBackend {
public:
    ContainerInfo info() const override {
        ContainerInfo i;
        i.format_name="H264/MP4";
        i.lossless=false; i.seekable=true; i.fps=30.0;
        i.frame_count=(uint32_t)frames_.size();
        i.description="H264 in MP4. Maximum compatibility. Stego use.";
        return i;
    }
    bool write_frame(const VideoFrame& f) override {
        frames_.push_back(f); return true;
    }
    VideoFrame read_frame(uint32_t id) override {
        if(id>=frames_.size()) return {};
        return frames_[id];
    }
    uint32_t frame_count() const override { return (uint32_t)frames_.size(); }
    bool seek(uint32_t) override { return true; }
    bool is_lossless() const override { return false; }
    std::vector<uint8_t> serialize() const override {
#ifdef CPB_REAL_FFMPEG
        // S4: 実H264エンコーダ (ffmpeg_backend.hpp)
        return ffmpeg_serialize_h264(frames_, 30.0);
#else
        RawBinaryBackend rb; for(auto& f:frames_) rb.write_frame(f);
        auto buf=rb.serialize(); buf[3]='M';
        return buf;
#endif
    }
    bool deserialize(const std::vector<uint8_t>& buf) override {
#ifdef CPB_REAL_FFMPEG
        return ffmpeg_deserialize_h264(buf, frames_);
#else
        auto copy=buf; if(copy.size()>=4) copy[3]='R';
        RawBinaryBackend rb; if(!rb.deserialize(copy)) return false;
        frames_.clear();
        for(uint32_t i=0;i<rb.frame_count();++i) frames_.push_back(rb.read_frame(i));
        return true;
#endif
    }
private:
    std::vector<VideoFrame> frames_;
};

// ── バックエンドファクトリ ──
enum class BackendType { RAW, PNG_SEQ, FFV1_MKV, H264_MP4, MP4_ATOM };

// ============================================================
// MP4AtomBackend — カスタムAtomステガノ (スタブ版)
// 本番: ffmpeg_backend.hpp の MP4AtomBackend を使用
// スタブ: 独自バイナリフォーマットで同等機能を提供
// ============================================================
class MP4AtomBackend : public IContainerBackend {
public:
    ContainerInfo info() const override {
        ContainerInfo i;
        i.format_name="MP4/udta";
        i.lossless=true; i.seekable=true; i.fps=fps_;
        i.frame_count=(uint32_t)frames_.size();
        i.description="CPB in MP4 udta atom. Lossless. Human-compatible.";
        return i;
    }
    bool write_frame(const VideoFrame& f) override { frames_.push_back(f); return true; }
    VideoFrame read_frame(uint32_t id) override {
        if(id>=frames_.size()) return {}; return frames_[id];
    }
    uint32_t frame_count() const override { return (uint32_t)frames_.size(); }
    bool seek(uint32_t) override { return true; }
    bool is_lossless() const override { return true; }

    std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> b;
        auto w32=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
        auto w64=[&](uint64_t v){for(int i=0;i<8;++i)b.push_back((uint8_t)((v>>(i*8))&0xFF));};
        for(char c: {'C','P','B','A','T','O','M','!'}) b.push_back((uint8_t)c);
        uint64_t fps_bits; memcpy(&fps_bits,&fps_,8); w64(fps_bits);
        w32((uint32_t)frames_.size());
        size_t atom_sz_pos = b.size();
        w32(0);
        { uint8_t atom_name[]={0xA9,'c','p','b'}; for(auto c:atom_name) b.push_back(c); }
        for(auto& f: frames_){
            w32(f.frame_id); w32((uint32_t)f.data.size());
            b.insert(b.end(),f.data.begin(),f.data.end());
        }
        uint32_t atom_sz=(uint32_t)(b.size()-atom_sz_pos);
        for(int i=0;i<4;++i) b[atom_sz_pos+i]=(uint8_t)((atom_sz>>(i*8))&0xFF);
        return b;
    }
    bool deserialize(const std::vector<uint8_t>& buf) override {
        if(buf.size()<20||memcmp(buf.data(),"CPBATOM!",8)!=0) return false;
        const uint8_t* p=buf.data()+8; const uint8_t* end=buf.data()+buf.size();
        uint64_t fps_bits=0; for(int i=0;i<8;++i) fps_bits|=(uint64_t)(*p++)<<(i*8);
        memcpy(&fps_,&fps_bits,8);
        uint32_t n=0; for(int i=0;i<4;++i) n|=(uint32_t)(*p++)<<(i*8);
        if(p+8>end) return false; p+=8;
        frames_.clear();
        for(uint32_t i=0;i<n;++i){
            if(p+8>end) break;
            uint32_t fid=0; for(int j=0;j<4;++j) fid|=(uint32_t)(*p++)<<(j*8);
            uint32_t sz=0;  for(int j=0;j<4;++j) sz|=(uint32_t)(*p++)<<(j*8);
            if(p+sz>end) return false;
            VideoFrame f; f.frame_id=fid; f.data.assign(p,p+sz); p+=sz;
            f.pts_sec=fid/fps_; frames_.push_back(std::move(f));
        }
        return (uint32_t)frames_.size()==n;
    }
private:
    double fps_ = 30.0;
    std::vector<VideoFrame> frames_;
};

inline std::unique_ptr<IContainerBackend> make_backend(BackendType t){
    switch(t){
    case BackendType::RAW:       return std::make_unique<RawBinaryBackend>();
    case BackendType::PNG_SEQ:   return std::make_unique<PNGSequenceBackend>();
    case BackendType::FFV1_MKV:  return std::make_unique<FFV1MKVBackend>();
    case BackendType::H264_MP4:  return std::make_unique<H264MP4Backend>();
    case BackendType::MP4_ATOM:  return std::make_unique<MP4AtomBackend>();
    }
    return std::make_unique<RawBinaryBackend>();
}
