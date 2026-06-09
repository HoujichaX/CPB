// carrier.cpp — CPBキャリア実装
#include "carrier.hpp"
#include <cstring>
#include <ctime>
#include <cstdio>

// CPBデータの識別マジック (キャリア内に埋め込む)
static const uint8_t CPB_CARRIER_SIG[8] = {
    0xCB, 0xC0, 0xC8, 0xB0, 0x43, 0x50, 0x42, 0x21  // \xCB...CPB!
};

// ── 小物ヘルパー ─────────────────────────────────────────────
static void push16le(std::vector<uint8_t>& v, uint16_t x){
    v.push_back(x&0xFF); v.push_back((x>>8)&0xFF);
}
static void push32le(std::vector<uint8_t>& v, uint32_t x){
    v.push_back(x&0xFF); v.push_back((x>>8)&0xFF);
    v.push_back((x>>16)&0xFF); v.push_back((x>>24)&0xFF);
}
static void push64le(std::vector<uint8_t>& v, uint64_t x){
    for(int i=0;i<8;i++) v.push_back((x>>(i*8))&0xFF);
}
static uint32_t read32le(const uint8_t* p){
    return p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24);
}

// ── CPB末尾シグネチャ付きブロック ────────────────────────────
// 形式: [CPB_CARRIER_SIG 8][data_size 8][cpb_data...][CPB_CARRIER_SIG 8]
static std::vector<uint8_t> make_cpb_block(const std::vector<uint8_t>& cpb){
    // 形式: [SIG 8][data ...][size 8][SIG 8]
    // → 末尾から読める: 末尾SIG → size → data → 先頭SIG
    std::vector<uint8_t> block;
    block.insert(block.end(), CPB_CARRIER_SIG, CPB_CARRIER_SIG+8);
    block.insert(block.end(), cpb.begin(), cpb.end());
    push64le(block, cpb.size());  // sizeは末尾SIGの直前
    block.insert(block.end(), CPB_CARRIER_SIG, CPB_CARRIER_SIG+8);
    return block;
}

// ── CRC32 (ZIP用) ─────────────────────────────────────────────
static uint32_t crc32_table[256];
static bool crc32_initialized = false;
static void init_crc32(){
    if(crc32_initialized) return;
    for(uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c=(c&1)?(0xEDB88320^(c>>1)):(c>>1);
        crc32_table[i]=c;
    }
    crc32_initialized=true;
}
static uint32_t crc32(const uint8_t* data, size_t len){
    init_crc32();
    uint32_t c=0xFFFFFFFF;
    for(size_t i=0;i<len;i++) c=crc32_table[(c^data[i])&0xFF]^(c>>8);
    return c^0xFFFFFFFF;
}

// ── ZIP キャリア ─────────────────────────────────────────────
static std::vector<uint8_t> wrap_zip(
    const std::vector<uint8_t>& cpb,
    const std::string& decoy_name)
{
    // 囮ファイルの内容
    const std::string decoy_content =
        "This file is part of a CPB archive.\r\n"
        "Use CPB tool to extract the actual content.\r\n";

    std::vector<uint8_t> out;
    const uint8_t* ddata = (const uint8_t*)decoy_content.data();
    size_t dsize = decoy_content.size();
    uint32_t dcrc = crc32(ddata, dsize);

    // DOS時刻 (固定値)
    uint16_t dostime = 0x6000; // 12:00:00
    uint16_t dosdate = 0x5345; // 2022/10/05

    // ── ローカルファイルヘッダ ──
    uint32_t local_offset = (uint32_t)out.size();
    out.push_back(0x50); out.push_back(0x4B); // PK
    out.push_back(0x03); out.push_back(0x04); // local file sig
    push16le(out, 20);      // version needed
    push16le(out, 0);       // flags
    push16le(out, 0);       // compression: stored
    push16le(out, dostime);
    push16le(out, dosdate);
    push32le(out, dcrc);
    push32le(out, (uint32_t)dsize);   // compressed size
    push32le(out, (uint32_t)dsize);   // uncompressed size
    push16le(out, (uint16_t)decoy_name.size());
    push16le(out, 0);  // extra field length
    out.insert(out.end(), decoy_name.begin(), decoy_name.end());
    out.insert(out.end(), ddata, ddata+dsize);

    // ── セントラルディレクトリ ──
    uint32_t cd_offset = (uint32_t)out.size();
    out.push_back(0x50); out.push_back(0x4B); // PK
    out.push_back(0x01); out.push_back(0x02); // central dir sig
    push16le(out, 20);      // version made by
    push16le(out, 20);      // version needed
    push16le(out, 0);       // flags
    push16le(out, 0);       // compression
    push16le(out, dostime);
    push16le(out, dosdate);
    push32le(out, dcrc);
    push32le(out, (uint32_t)dsize);
    push32le(out, (uint32_t)dsize);
    push16le(out, (uint16_t)decoy_name.size());
    push16le(out, 0);  // extra
    push16le(out, 0);  // comment
    push16le(out, 0);  // disk start
    push16le(out, 0);  // int attrib
    push32le(out, 0);  // ext attrib
    push32le(out, local_offset);
    out.insert(out.end(), decoy_name.begin(), decoy_name.end());

    uint32_t cd_size = (uint32_t)out.size() - cd_offset;

    // ── End of Central Directory ──
    out.push_back(0x50); out.push_back(0x4B); // PK
    out.push_back(0x05); out.push_back(0x06); // EOCD sig
    push16le(out, 0);   // disk number
    push16le(out, 0);   // CD start disk
    push16le(out, 1);   // entries on disk
    push16le(out, 1);   // total entries
    push32le(out, cd_size);
    push32le(out, cd_offset);
    push16le(out, 0);   // comment length

    // ── CPBブロックを末尾に追記 ──
    auto block = make_cpb_block(cpb);
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

// ── MP4 キャリア ─────────────────────────────────────────────
static std::vector<uint8_t> wrap_mp4(const std::vector<uint8_t>& cpb){
    // 最小限のMP4: ftyp box + 壊れたmdat box
    std::vector<uint8_t> out;

    // ftyp box
    const uint8_t ftyp[] = {
        0x00,0x00,0x00,0x14, 'f','t','y','p',  // size=20, type
        'i','s','o','m',                         // major brand
        0x00,0x00,0x02,0x00,                     // minor version
        'i','s','o','m','m','p','4','1'          // compatible brands
    };
    out.insert(out.end(), ftyp, ftyp+sizeof(ftyp));

    // CPBブロック (mdatに見えるよう包む)
    // mdat box: [size 4][mdat 4][cpb_block]
    auto block = make_cpb_block(cpb);
    uint32_t mdat_size = (uint32_t)(8 + block.size());
    push32le(out, mdat_size);
    out.push_back('m'); out.push_back('d'); out.push_back('a'); out.push_back('t');
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

// ── PDF キャリア ─────────────────────────────────────────────
static std::vector<uint8_t> wrap_pdf(const std::vector<uint8_t>& cpb){
    // 最小限のPDF + CPBストリームとして埋め込む
    std::string pdf_header =
        "%PDF-1.4\n"
        "%\xe2\xe3\xcf\xd3\n"  // binary comment (PDF header hint)
        "1 0 obj\n"
        "<< /Type /Catalog /Pages 2 0 R >>\n"
        "endobj\n"
        "2 0 obj\n"
        "<< /Type /Pages /Kids [] /Count 0 >>\n"
        "endobj\n";

    std::vector<uint8_t> out(pdf_header.begin(), pdf_header.end());

    // CPBをバイナリストリームとして埋め込む
    auto block = make_cpb_block(cpb);
    std::string obj_header =
        "3 0 obj\n"
        "<< /Length " + std::to_string(block.size()) + " >>\n"
        "stream\n";
    out.insert(out.end(), obj_header.begin(), obj_header.end());
    // CPBデータの内容 (プレースホルダー)
    std::string dummy_content(block.size(), 'X');
    out.insert(out.end(), dummy_content.begin(), dummy_content.end());
    const std::string obj_footer = "\nendstream\nendobj\n";
    out.insert(out.end(), obj_footer.begin(), obj_footer.end());

    // xref + trailer
    uint32_t xref_pos = (uint32_t)out.size();
    std::string xref =
        "xref\n"
        "0 4\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "trailer\n"
        "<< /Size 4 /Root 1 0 R >>\n"
        "startxref\n" +
        std::to_string(xref_pos) + "\n"
        "%%EOF\n";
    out.insert(out.end(), xref.begin(), xref.end());
    // PDFはEOFの後にCPBブロックを追記
    auto block2 = make_cpb_block(cpb);
    out.insert(out.end(), block2.begin(), block2.end());
    return out;
}

// ── PNG キャリア ─────────────────────────────────────────────
static std::vector<uint8_t> wrap_png(const std::vector<uint8_t>& cpb){
    // 最小1x1 PNGヘッダ + IENDの後にCPBブロック
    static const uint8_t min_png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A, // PNG signature
        0x00,0x00,0x00,0x0D,'I','H','D','R',
        0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x01, // 1x1
        0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,0xDE, // bit depth, color type, crc
        0x00,0x00,0x00,0x0C,'I','D','A','T',
        0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,0x00,0x00,0x02,0x00,0x01, // minimal IDAT
        0xE2,0x21,0xBC,0x33,
        0x00,0x00,0x00,0x00,'I','E','N','D',0xAE,0x42,0x60,0x82  // IEND
    };
    std::vector<uint8_t> out(min_png, min_png+sizeof(min_png));
    // IENDの後にCPBブロックを追記
    auto block = make_cpb_block(cpb);
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

// ── 公開API ──────────────────────────────────────────────────
std::vector<uint8_t> carrier_wrap(
    const std::vector<uint8_t>& cpb_data,
    CarrierFormat fmt,
    const std::string& decoy_name)
{
    switch(fmt){
    case CarrierFormat::ZIP: return wrap_zip(cpb_data, decoy_name.empty()?"readme.txt":decoy_name);
    case CarrierFormat::MP4: return wrap_mp4(cpb_data);
    case CarrierFormat::PDF: return wrap_pdf(cpb_data);
    case CarrierFormat::PNG: return wrap_png(cpb_data);
    case CarrierFormat::RAW:
    case CarrierFormat::CPB:
    default:
        // ネイティブ: そのまま返す
        return cpb_data;
    }
}

std::vector<uint8_t> carrier_unwrap(const std::vector<uint8_t>& file_data){
    if(file_data.size()<16) return file_data;

    // CPBシグネチャを末尾から検索
    // 末尾8バイトがCPB_CARRIER_SIG → シグネチャあり
    const size_t sz = file_data.size();
    // 末尾から: [SIG 8][size 8][data(size)][SIG 8]
    if(sz >= 24 && memcmp(file_data.data()+sz-8, CPB_CARRIER_SIG, 8)==0){
        // 末尾SIG(8) → その前size(8)
        uint64_t data_size = 0;
        const uint8_t* sp = file_data.data() + sz - 8 - 8; // sizeの位置
        for(int i=0;i<8;i++) data_size|=(uint64_t(sp[i])<<(i*8));
        // さらに前data(data_size) → 先頭SIG(8)
        if(data_size + 24 <= sz){
            const uint8_t* data_start = sp - data_size;        // dataの開始
            const uint8_t* head_sig   = data_start - 8;        // 先頭SIG
            if(head_sig >= file_data.data() &&
               memcmp(head_sig, CPB_CARRIER_SIG, 8)==0){
                return std::vector<uint8_t>(data_start, data_start+data_size);
            }
        }
    }

    // シグネチャなし → ネイティブCPBとして返す
    return file_data;
}

bool is_cpb_carrier(const std::vector<uint8_t>& file_data){
    if(file_data.size()<24) return false;
    size_t sz=file_data.size();
    if(memcmp(file_data.data()+sz-8, CPB_CARRIER_SIG, 8)!=0) return false;
    uint64_t data_size=0;
    const uint8_t* sp=file_data.data()+sz-8-8;
    for(int i=0;i<8;i++) data_size|=(uint64_t(sp[i])<<(i*8));
    if(data_size+24>sz) return false;
    const uint8_t* data_start=sp-data_size;
    const uint8_t* head_sig=data_start-8;
    return head_sig>=file_data.data() && memcmp(head_sig,CPB_CARRIER_SIG,8)==0;
}
