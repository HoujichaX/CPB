#pragma once
// frame_io_soft.hpp — OpenCV不要のソフトウェアフレームI/O
// NO_FRAME_IO が定義されている場合に frame_io.hpp の代わりに使用
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cstring>

constexpr int    RVAC_FRAME_WIDTH    = 1920;
constexpr int    RVAC_FRAME_HEIGHT   = 1080;
constexpr size_t RVAC_FRAME_CAPACITY =
    (size_t)RVAC_FRAME_WIDTH * RVAC_FRAME_HEIGHT;

// OpenCVのcv::Matの代わりに使う生フレーム型
struct RawFrame {
    std::vector<uint8_t> pixels;  // RVAC_FRAME_CAPACITY バイト
    int width  = RVAC_FRAME_WIDTH;
    int height = RVAC_FRAME_HEIGHT;
};

// Blob → RawFrame
inline RawFrame blob_to_raw_frame(const std::vector<uint8_t>& blob)
{
    const size_t header = sizeof(uint64_t);
    if(header + blob.size() > RVAC_FRAME_CAPACITY)
        throw std::runtime_error("Blob too large for one frame");

    RawFrame f;
    f.pixels.assign(RVAC_FRAME_CAPACITY, 0);
    uint64_t sz = blob.size();
    std::memcpy(f.pixels.data(), &sz, header);
    std::memcpy(f.pixels.data() + header, blob.data(), blob.size());
    return f;
}

// RawFrame → Blob
inline std::vector<uint8_t> raw_frame_to_blob(const RawFrame& f)
{
    if(f.pixels.size() < sizeof(uint64_t))
        throw std::runtime_error("Frame too small");
    uint64_t sz = 0;
    std::memcpy(&sz, f.pixels.data(), sizeof(uint64_t));
    const size_t cap = f.pixels.size() - sizeof(uint64_t);
    if(sz > cap) throw std::runtime_error("Invalid payload size");
    return {f.pixels.data() + sizeof(uint64_t),
            f.pixels.data() + sizeof(uint64_t) + sz};
}
