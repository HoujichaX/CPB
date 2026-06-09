#include "dsl_vm.hpp"
#include <cstring>
#include <stdexcept>

// ============================================================
// DSL 命令フォーマット
//
//  0x01  OUTPUT  [size:8]
//        → 出力バッファを size バイトで確保 (0埋め)
//
//  0x02  RAW     [blob_id:4][src_offset:8][dst:8][len:4]
//        → blobs[blob_id].data[src_offset .. src_offset+len]
//           を out[dst..] へコピー
//
//  0x03  COPY    [src:8][dst:8][len:4]
//        → out[src..src+len] を out[dst..] へコピー
//           (src と dst が重なる場合も正しく動作)
//
//  0x04  FILL    [dst:8][len:4][byte:1]
//        → out[dst..dst+len] を byte で埋める
//           ゼロパディング・繰り返しバイトに最適
//
//  0xFF  END
//        → 命令列終端、out を返す
// ============================================================

std::vector<Byte> run_vm(
    const std::vector<Byte>& code,
    const std::vector<Blob>& blobs)
{
    std::vector<Byte> out;
    size_t pc = 0;

    auto r8  = [&](){ return code[pc++]; };
    auto r32 = [&](){ uint32_t v; memcpy(&v,&code[pc],4); pc+=4; return v; };
    auto r64 = [&](){ uint64_t v; memcpy(&v,&code[pc],8); pc+=8; return v; };

    while (pc < code.size()) {
        switch (r8()) {

        case 0x01: { // OUTPUT [size:8]
            out.assign(r64(), 0);
            break;
        }

        case 0x02: { // RAW [blob_id:4][src_offset:8][dst:8][len:4]
            uint32_t id         = r32();
            uint64_t src_offset = r64();
            uint64_t dst        = r64();
            uint32_t len        = r32();
            const auto& b = blobs.at(id).data;
            if (src_offset + len > b.size())
                throw std::runtime_error("DSL RAW: out of blob bounds");
            if (dst + len > out.size())
                throw std::runtime_error("DSL RAW: out of output bounds");
            memcpy(out.data() + dst, b.data() + src_offset, len);
            break;
        }

        case 0x03: { // COPY [src:8][dst:8][len:4]
            uint64_t s = r64(), d = r64();
            uint32_t l = r32();
            if (s + l > out.size() || d + l > out.size())
                throw std::runtime_error("DSL COPY: out of bounds");
            memmove(out.data() + d, out.data() + s, l);
            break;
        }

        case 0x04: { // FILL [dst:8][len:4][byte:1]
            uint64_t dst = r64();
            uint32_t len = r32();
            uint8_t  val = r8();
            if (dst + len > out.size())
                throw std::runtime_error("DSL FILL: out of bounds");
            memset(out.data() + dst, val, len);
            break;
        }

        case 0xFF:
            return out;

        default:
            throw std::runtime_error("DSL: unknown opcode");
        }
    }
    return out;
}
