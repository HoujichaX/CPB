# CPB — Cocoa Powder Bottle
![License](https://img.shields.io/badge/license-Apache%202.0-c45e28?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B17-f5a623?style=flat-square&logo=cplusplus&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Windows-0078d4?style=flat-square&logo=windows&logoColor=white)
![Build](https://img.shields.io/badge/build-MSVC-5c2d91?style=flat-square&logo=visualstudio&logoColor=white)
![Status](https://img.shields.io/badge/status-stable-4a9c3f?style=flat-square)

**A multi-layer data compression and protection system that uses video and audio containers as its vessel.**

```
Peak decode:     22,687 MB/s  (saturates NVMe SSD bandwidth)
Best compression: 0.4%        (L5 dictionary cache, 2nd pass)
Overhead:         +56 B       (vs ZIP's +883 B per file)
Self-repair:      ✓           (Reed-Solomon error correction)
Output formats:   .cpb / .zip / .mp4 / .pdf / .png
```

---

## Why CPB?

### 1. Asymmetric Speed — Compress once, decode millions of times

CPB is built for the real world where **compression happens once, but decompression happens millions of times**. Game downloads, software updates, CDN delivery — the decode path is what matters.

| Tool | Decode speed | Compression | Self-repair |
|------|-------------|-------------|-------------|
| ZIP  | ~400 MB/s   | decent      | ✗           |
| 7z   | ~200 MB/s   | excellent   | ✗           |
| zstd | ~1.8 GB/s   | excellent   | ✗           |
| CPB  | **22 GB/s** | excellent   | **✓**       |

### 2. Self-Repairing Archives — Data that heals itself

Every CPB archive includes Reed-Solomon error correction codes. When bits flip — on spinning disks, aging SSDs, or unreliable networks — CPB detects and repairs the damage automatically.

No more "archive is corrupted" error dialogs. No more re-downloading 50 GB game patches.

| RS Preset | Protection level | Use case |
|-----------|-----------------|----------|
| STANDARD  | Balanced        | General purpose (default) |
| MAX       | Maximum         | Long-term archival |
| LIGHT     | Fast            | Speed-critical delivery |

### 3. Five Output Formats — Your data, any disguise

CPB data can be wrapped in multiple carrier formats:

| Format | Extension | What it looks like |
|--------|-----------|-------------------|
| CPB    | .cpb      | Native format (default) |
| ZIP    | .zip      | Standard ZIP — opens with Windows right-click |
| MP4    | .mp4      | Looks like a video file |
| PDF    | .pdf      | Looks like a document |
| PNG    | .png      | Looks like an image |

Unpacking auto-detects the carrier — no format selection needed.

### 4. Dictionary Intelligence — The more it learns, the smaller it gets

CPB uses two dictionary systems that work together:

**L3 — Genre DSL (phrase dictionary)**
Auto-detects file type (JSON / CSV / XML / binary) and converts recurring patterns to compact DSL instructions. Train with `.cpbdict` files for domain-specific compression.

**L5 — GenDict (exact-match cache)**
On first encounter, a file compresses normally. On second encounter, CPB recognizes the hash and returns a cached result in just ~279 bytes. The cache grows as you pack — the more you use it, the better it gets.

```
1st pass:  11,429 B → 2,319 B  (20.3%)
2nd pass:  11,429 B →   279 B  ( 2.4%)  ← cache hit
```

### 5. Multi-Dimensional Shuffle — 4D to 16D scrambling

L4 shuffles byte sequences across up to 16 independent dimensions, each with its own sub-seed. Higher dimensions mean larger key spaces but slower processing.

| Setting | Dimensions | Encode speed |
|---------|-----------|-------------|
| 4D  | Z · Y · X · W | 24 MB/s |
| 8D  | + color · bit-plane · audio-ch · audio-band | 12 MB/s |
| 12D | + block-pos · DCT · time-window | 8 MB/s |
| 16D | + edge-dir · texture · energy | 6 MB/s |

---

## Pipeline Architecture

```
Input Data
  ↓  L5 GenDict    — Exact-match cache (learning mode)
  ↓  L3 Genre DSL  — Domain-specific phrase dictionary
  ↓  L2 Compress   — LZMS / MSZIP / XPRESS / XPRESS_HUFF
  ↓  L1 Protect    — Reed-Solomon error correction
  ↓  L4 4D Shuffle — Multi-dimensional scrambling (4D–16D)
  ↓  FIDX Search   — Full-text search index
  ↓
CPB Container (.cpb / .zip / .mp4 / .pdf / .png)
```

Each layer can be independently toggled on/off. Profiles provide pre-configured combinations:

| Profile  | Pipeline | Use case |
|----------|----------|----------|
| STANDARD | L5→L3→L2→L1→L4→FIDX | General purpose (default) |
| ARCHIVE  | L1→L4→FIDX | Maximum protection |
| DEFENSE  | L3→L2→L4→L1→FIDX | Multi-layer protection |
| LEARN    | L5→L3→L2→L1→L4→FIDX | Dictionary learning enabled |
| AI_PACKET | L5→L3→L1→FIDX | AI communication |
| STEGO    | L2→L1→L4 | Minimal footprint |
| CUSTOM   | User choice | Toggle any layer on/off |

---

## Benchmarks

**Environment:** Intel Core i7-12700 (2.10 GHz) / 16 GB RAM / NVMe SSD / Windows

### L2 Compression Algorithms (64KB JSON)

| Algorithm | Ratio | Encode | Decode |
|-----------|-------|--------|--------|
| LZMS / MSZIP | 0.6% | 891 MB/s | 608 MB/s |
| RLE | 100.8% | 240 MB/s | 611 MB/s |
| NONE (passthrough) | 100.0% | 12,435 MB/s | 12,498 MB/s |

### Full Pipeline Comparison (64KB JSON)

| Configuration | Ratio | Encode speed |
|--------------|-------|-------------|
| L2 only | 0.6% | 629 MB/s |
| STANDARD (4D) | 0.8% | 4.2 MB/s |
| STANDARD + L3 dict | 1.2% | 148 MB/s |
| L2 + L3 dict | 0.8% | 161 MB/s |

### L5 LEARN Cache Effect

| Pass | Ratio | Decode speed |
|------|-------|-------------|
| 1st (cold) | 1.2% | 218 MB/s |
| 2nd (cache hit) | **0.4%** | **1,857 MB/s** |
| 3rd | 0.4% | 2,531 MB/s |

### Peak Performance

| Metric | Value | Condition |
|--------|-------|-----------|
| Best compression | 0.0% | ZERO 1MB → 270 B |
| Fastest encode | 12,435 MB/s | L2 NONE |
| Fastest decode | 22,687 MB/s | ZERO 1MB |
| L5 LEARN (2nd) | 0.4% / 1,857 MB/s | 64KB JSON |

### Archive Overhead (per file)

```
CPB (.cpb):  +0 B / +56 B / +93 B
ZIP (.zip):  +224 B / +883 B
→ CPB archives are 10–15x lighter than ZIP
```

---

## Applications

### Three GUI Apps

| App | Purpose |
|-----|---------|
| **cpb_gui.exe** | Main app — PACK / UNPACK with layer toggles and profiles |
| **cpb_train.exe** | Dictionary trainer — create L3 (.cpbdict) and L5 (.dict) dictionaries |
| **cpb_reader.exe** | Archive browser — search, preview, and extract files |

### Quick Workflow

1. **Pack:** Drop files into cpb_gui.exe → select profile → PACK
2. **Train:** Feed sample files into cpb_train.exe → generate dictionaries
3. **Browse:** Drop .cpb into cpb_reader.exe → search → preview → extract

---

## Use Cases

### Game Distribution
Compress once at the studio. Millions of players decompress at SSD speed. RS codes eliminate "corrupted download" support tickets. Train dictionaries on your game's assets for maximum compression.

### Software Updates
Delta patches compressed with trained dictionaries. Self-repairing delivery over unreliable networks. Multiple carrier formats for any delivery channel.

### Data Archival
Long-term storage with Reed-Solomon protection against bit-rot. Multiple carrier formats ensure future readability. L5 cache makes incremental backups near-zero cost.

### Content Delivery
Wrap data as .mp4 and stream through existing video CDN infrastructure. Or wrap as .zip for standard download. Same data, different packaging.

---

## Building

Visual Studio Developer Command Prompt (x64):

```bat
:: Main GUI app
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_gui.cpp /Fe:cpb.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib

:: Dictionary trainer
cl /std:c++17 /O2 /EHsc /nologo /utf-8 ^
   cpb_train.cpp /Fe:cpb_train.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib

:: Archive browser
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_reader.cpp /Fe:cpb_reader.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib
```

### Requirements

- C++17 compiler (MSVC 2019+)
- Windows SDK (Compression API, common controls)
- No external dependencies (no OpenCV, no ffmpeg, no zstd)

### Platforms

- **Windows** — primary platform (Win32 GUI + CLI)
- **Linux** — core library (no GUI)

---

## Dictionary Ecosystem

### Create

Use cpb_train.exe to generate dictionaries from sample data:

| Dictionary | Extension | Layer | Effect |
|-----------|-----------|-------|--------|
| Phrase dictionary | .cpbdict | L3 | Replaces recurring patterns with compact references |
| Cache dictionary | .dict | L5 | Stores exact-match compressed results for instant replay |

### Share

Dictionaries are single files. Distribute alongside your archives, or keep them private as a competitive advantage — your trained dictionaries are your compression edge.

### Compose

Load both L3 and L5 dictionaries simultaneously for maximum compression. The learning system grows with each pack operation.

---

## License

**Dual License:**

- **CPB Core** (compression pipeline, RS codes, container formats, GUI apps):
  Apache License 2.0 — free for any use, commercial or otherwise.

- **Trained Dictionaries & Dictionary Training Tools**:
  Commercial license available. Contact for enterprise dictionary packages optimized for your specific data workloads.

The core is fully functional without commercial dictionaries. Train your own for free using cpb_train.exe.

---

## Comparison

|                        | CPB           | ZIP    | 7z     | zstd    | PAR2  |
|------------------------|--------------|--------|--------|---------|-------|
| Decode speed           | **22 GB/s**  | 400 MB | 200 MB | 1.8 GB  | —     |
| Compression            | ◎            | ○      | ◎      | ◎       | —     |
| Error repair           | **✓ RS**     | ✗      | ✗      | ✗       | ✓     |
| Carrier formats        | **5 types**  | 1      | 1      | 1       | —     |
| Dictionary training    | **✓ L3+L5**  | ✗      | ✗      | △       | ✗     |
| Multi-dim shuffle      | **4D–16D**   | ✗      | ✗      | ✗       | ✗     |
| Full-text search       | **✓ FIDX**   | ✗      | ✗      | ✗       | ✗     |
| Archive overhead       | +56 B        | +883 B | ~1 KB  | —       | —     |

---

## Project Status

- Pipeline core: **stable**
- GUI apps (3): **stable**
- Dictionary system: **stable**
- Reed-Solomon: **stable**
- 4D–16D shuffle: **stable**
- Genre DSL: partial (stubs for some media types)
- FIDX search: **stable**

---

*CPB is a sister project of CMF (Chocolate Muffin), a cultural-anchoring steganographic system available under separate license to qualified organizations.*
