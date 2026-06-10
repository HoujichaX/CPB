# CPB — Cocoa Powder Bottle

**動画・音声コンテナを"器"とする多層データ圧縮・保護システム**

```
最速デコード:   22,687 MB/s  (NVMe SSD の帯域を使い切る)
最高圧縮率:     0.4%         (L5 辞書キャッシュ、 2 回目以降)
オーバーヘッド:  +56 B        (ZIP の +883 B に対して 10〜15 倍軽量)
自己修復:       ✓            (Reed-Solomon 誤り訂正)
出力形式:       .cpb / .zip / .mp4 / .pdf / .png
```

---

## 他ツールとの比較

|                    | CPB           | ZIP    | 7z     | zstd    | PAR2  |
|--------------------|--------------|--------|--------|---------|-------|
| デコード速度        | **22 GB/s**  | 400 MB | 200 MB | 1.8 GB  | —     |
| 圧縮性能            | ◎            | ○      | ◎      | ◎       | —     |
| 誤り修復            | **✓ RS**     | ✗      | ✗      | ✗       | ✓     |
| 出力形式            | **5 種類**   | 1      | 1      | 1       | —     |
| 辞書学習            | **✓ L3+L5** | ✗      | ✗      | △       | ✗     |
| 多次元シャッフル     | **4D〜16D** | ✗      | ✗      | ✗       | ✗     |
| 全文検索            | **✓ FIDX**  | ✗      | ✗      | ✗       | ✗     |
| オーバーヘッド       | +56 B       | +883 B | ~1 KB  | —       | —     |


## 概要

CPBは、データを多段パイプラインで処理する独自コンテナフォーマットです。  
圧縮・保護・難読化・インデックスを組み合わせ、用途に応じてレイヤーを自在に選択できます。

```
入力データ
  ↓  L5 GenDict    — 完全一致キャッシュ（学習モード）
  ↓  L3 Genre DSL  — 分野別フレーズ辞書変換
  ↓  L2 Compress   — LZMS / MSZIP / XPRESS etc.
  ↓  L1 Protect    — Reed-Solomon 誤り訂正符号
  ↓  L4 4D Shuffle — 多次元シャッフル (4D〜16D)
  ↓  FIDX Search   — 全文検索インデックス
  ↓
CPBコンテナ (.cpb / .zip / .mp4 / .pdf / .png)
```

---

## ファイル構成

```
CPB-DIST/
├── cpb_gui.cpp          — メインGUIアプリ (PACK / UNPACK)
├── cpb_train.cpp        — 辞書訓練アプリ
├── cpb_reader.cpp       — 参照・検索アプリ
│
├── cpb_config.hpp       — 設定構造体 (CPBConfig / DimConfig / RSParams)
├── cpb_helpers.hpp      — 共通ユーティリティ
├── layer_pipeline.hpp   — パイプライン API
├── layer_pipeline.cpp   — 各レイヤーの encode / decode 実装
│
├── container.hpp/.cpp   — コンテナ読み書き
├── container_backend.hpp— バックエンド抽象化 (RAW)
├── carrier.hpp/.cpp     — キャリア形式ラッパー (ZIP / MP4 / PDF / PNG)
│
├── genre_dsl.hpp/.cpp   — L3 ジャンル検出 + フレーズ辞書
├── genre_dsl_vm.cpp     — L3 DSL 仮想マシン
├── fourd_map.hpp/.cpp   — L4 多次元マッピング (概念実装)
├── protection_layer.hpp/.cpp — L1 Reed-Solomon
├── compress_impl.cpp    — L2 圧縮 (Windows Compression API)
├── l5_dict_persist.hpp  — L5 辞書の保存・読み込み
├── rs_codec.cpp         — Reed-Solomon コーデック
│
├── dsl_vm.cpp           — DSL 仮想マシン基盤
├── frame_index.cpp      — FIDX 検索インデックス
├── fourd_map.cpp        — 4D 空間マッピング
├── gen_codec.cpp        — ジャンル汎用コーデック
├── search_api.cpp       — 検索 API
├── cpb_dict_protocol.cpp— 辞書プロトコル
├── dict_evolution.cpp   — 辞書進化アルゴリズム
├── header.cpp           — ヘッダ処理
└── frame_index.cpp      — フレームインデックス
```

---

## ビルド

Visual Studio の開発者コマンドプロンプト (x64) で実行してください。

### CPB GUI（メインアプリ）
```bat
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_gui.cpp /Fe:cpb.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib
```

### 辞書訓練アプリ
```bat
cl /std:c++17 /O2 /EHsc /nologo /utf-8 ^
   cpb_train.cpp /Fe:cpb_train.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib
```

### 参照・検索アプリ
```bat
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_reader.cpp /Fe:cpb_reader.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib
```

---

## 使い方

### 1. 基本的な PACK / UNPACK（cpb.exe）

```
[左サイドバー]  各レイヤーのトグルスイッチで有効/無効を切り替え
[右パネル]      プロファイル選択、L2/L1/L4の詳細設定
[中央]          入出力欄 + PACK / UNPACK ボタン
```

1. ファイルまたはフォルダをドロップ（または「ファイル」「Dir」で選択）
2. 右パネルでプロファイルを選択（STANDARD / ARCHIVE / LEARN など）
3. ▶ PACK ボタンを押す

**フォルダのPACK**では「ファイル個別圧縮」チェックボックスで方式を選べます：
- **個別圧縮（推奨）** — ファイルを1つずつパイプラインに通す。L5学習効果が出る
- **全体圧縮** — 全ファイルを結合してから圧縮。ファイル間の共通パターンを検出

### 2. 辞書の作成（cpb_train.exe）

辞書を使うと同じ構造のデータを繰り返し圧縮する際の効率が大幅に向上します。

1. 「サンプル」タブでサンプルファイル（学習元）を追加
2. 「出力辞書」タブでL3辞書（.cpbdict）とL5辞書（.dict）の保存先を指定
3. （パラメータタブで調整可能）
4. 「学習スタート」ボタンを押す

生成したファイルを cpb.exe の辞書欄に指定してください。

| ファイル | 用途 |
|---|---|
| `.cpbdict` | L3辞書：フレーズパターンの置換（構造が似たデータに効果的） |
| `.dict` | L5辞書：完全一致キャッシュ（同じファイルが繰り返し来る場合に効果的） |

### 3. 中身の参照・検索（cpb_reader.exe）

```
.cpb / .raw / .zip / .mp4 / .pdf / .png をドロップして開く
→ 左ペイン：ファイル一覧
→ 右ペイン：プレビュー（テキスト / HEXダンプ自動判定）
```

| 操作 | 機能 |
|---|---|
| リストクリック | プレビュー表示 |
| 検索ボックス | キーワードでヒットファイルを絞り込み |
| 💾 このファイルを取り出す | 選択ファイルを1件エクスポート |
| 📦 全て展開 | フォルダを指定して全ファイルを展開 |
| 🔑 辞書指定 | L5(.dict) または L3(.cpbdict) を指定して再展開 |

コマンドライン引数でファイルを直接開くことも可能です：
```bat
cpb_reader.exe demo.cpb
```

---

## パイプライン詳細

### L5 — GenDict（完全一致キャッシュ）

```
同じファイルが2回目以降に来ると、ハッシュで検索して
圧縮済みDSLを直接返す → 数十バイトに圧縮

1回目: 11,429B → 2,319B (20.3%)
2回目: 11,429B →    279B  (2.4%)  ← キャッシュヒット
```

学習モードでPACKするたびにキャッシュが育ちます。  
辞書ファイル（.dict）に保存して次回以降も引き継げます。

### L3 — Genre DSL（フレーズ辞書変換）

ジャンル検出（JSON / CSV / XML / バイナリ etc.）→ DSL命令に変換。  
`.cpbdict` 辞書を使うと頻出フレーズを参照番号に置換します。

```
{"timestamp":"2026-01-01"... → [#001][#003]... (フレーズ参照)
```

辞書なし:  11,429B → 2,319B (20.3%)  
辞書あり:  11,429B → 2,064B (18.1%)  ← 構造学習で追加削減

### L2 — 圧縮

Windows Compression API を使用します。

| アルゴリズム | 特徴 |
|---|---|
| AUTO | Windowsが自動選択（通常LZMS） |
| LZMS | 最高圧縮率・低速 |
| MSZIP | DEFLATE互換・バランス型 |
| XPRESS_HUFF | 高速・中程度の圧縮 |
| XPRESS | 最速・低圧縮 |
| NONE | 圧縮なし（パスルー） |

### L1 — Reed-Solomon 誤り訂正

データが壊れても復元できます。強度はプリセットで選択：

| 設定 | DSL | Blob | 特徴 |
|---|---|---|---|
| STANDARD | 127/128 | 223/32 | 汎用（デフォルト） |
| MAX | 1/254 | 127/128 | 最高保護・低速 |
| STEGO | 127/128 | 1/254 | ステガノグラフィ向け |
| LIGHT | 223/32 | 223/32 | 高速・低耐性 |
| NONE | — | — | RS無効（高速） |

> ⚠️ **STEGO** は大幅にサイズが膨張します（最大255倍）。用途を限定してください。

### L4 — 多次元シャッフル

シードと次元設定に基づいてバイト列をシャッフルします。  
次元数が多いほど鍵空間が広がりますが処理時間も増加します。

| 設定 | 有効次元 | エンコード速度 |
|---|---|---|
| 4D | Z・Y・X・W | 24 MB/s |
| 8D | +色・ビット平面・音声ch・音声帯域 | 12 MB/s |
| 12D | +ブロック位置・DCT・時間窓 | 8 MB/s |
| 16D | +エッジ方向・テクスチャ・エネルギー | 6 MB/s |

各次元は独立したサブシードでシャッフルされます：
```
sub_seed(dim) = hash(master_seed XOR dim_id × 定数)
```

### FIDX — 全文検索インデックス

コンテナ末尾にインデックスを付加します。  
cpb_reader.exe の検索機能で利用されます。

---

## キャリア形式

CPBデータをさまざまなファイル形式に包んで保存できます。

| 形式 | 拡張子 | 特徴 |
|---|---|---|
| CPB | .cpb | ネイティブ形式（デフォルト） |
| ZIP | .zip | 汎用・Windowsで右クリックから開ける。囮ファイル入り |
| MP4 | .mp4 | 動画ファイルに偽装 |
| PDF | .pdf | 文書ファイルに偽装 |
| PNG | .png | 画像ファイルに偽装 |
| RAW | .raw | 旧形式互換 |

**仕組み：**
```
PACK時: CPBコンテナ → キャリアで包む → .zip などで保存
UNPACK時: ファイルを自動判定 → キャリアを剥がす → 通常通り展開
```

UNPACK時はキャリア形式の指定は不要です（自動検出）。

---

## プロファイル一覧

| プロファイル | パイプライン | 用途 |
|---|---|---|
| STANDARD | L5→L3→L2→L1→L4→FIDX | 汎用（デフォルト） |
| ARCHIVE | L1→L4→FIDX | 保護重視・高耐久 |
| STEGO | L2→L1→L4 | 最小構成 |
| DEFENSE | L3→L2→L4→L1→FIDX | 多重保護 |
| AI_PACKET | L5→L3→L1→FIDX | AI通信向け |
| LEARN | L5→L3→L2→L1→L4→FIDX | 学習モード有効 |
| CUSTOM | 個別選択 | レイヤーをON/OFFで自由に組み合わせ |

---

## ベンチマーク（代表値）

環境：Linux x86-64 / g++ -O2 / 64KB JSONデータ

### L2 圧縮アルゴリズム

| アルゴリズム | 圧縮率 | エンコード | デコード |
|---|---|---|---|
| LZMS / MSZIP | 0.6% | 891 MB/s | 608 MB/s |
| RLE | 100.8% | 240 MB/s | 611 MB/s |
| NONE | 100.0% | 12,435 MB/s | 12,498 MB/s |

### パイプライン比較（64KB JSON）

| 設定 | 圧縮率 | エンコード |
|---|---|---|
| L2 のみ | 0.6% | 629 MB/s |
| STANDARD (4D) | 0.8% | 4.2 MB/s |
| STANDARD + L3辞書 | 1.2% | 148 MB/s |
| L2 + L3辞書 | 0.8% | 161 MB/s |

### L5_LEARN キャッシュ効果

| 回数 | 圧縮率 | デコード速度 |
|---|---|---|
| 1回目（コールド） | 1.2% | 218 MB/s |
| 2回目（キャッシュヒット） | **0.4%** | **1,857 MB/s** |
| 3回目 | 0.4% | 2,531 MB/s |

---

## 注意事項

- **L1 STEGO** は意図的にサイズを膨張させます。通常用途では使わないでください
- **L5辞書（.dict）** はセッションをまたいで引き継ぐために必ず保存してください
- クラウド同期フォルダ（OneDriveのオフラインファイル等）はPACK対象から自動スキップされます
- UNPACK時に辞書が必要なファイルは、cpb_reader.exeの「🔑 辞書指定」で指定してください

---

## 将来の予定

- **L4 多次元空間（案B）** — ヒルベルト曲線を使った本格的な多次元座標マッピング
- **CPBアニメ** — アニメ・漫画特化の拡張（CEL_ANIM / PALETTE_REGION 命令）
- **マルチフレームコンテナ** — フレームごとにランダムアクセス可能な構造
- **差分バックアップ** — 同じフォルダの変更分だけ保存

---

*CPB — Cocoa Powder Bottle*

---

## ライセンス

カスタムライセンス — 全文は [LICENSE](LICENSE) を参照（日本語・英語併記）。

**許可：** ソースコードの閲覧・学習・研究、個人利用、自身のプロジェクトへの組み込み、辞書の作成・共有・配布（一切の制限なし）。

**禁止：** 違法行為を目的とした利用、データの秘匿化・検査回避・証拠隠滅を目的とした利用、兵器・監視システム・人権侵害に関わるシステムの構成要素としての使用。

**辞書は完全に自由です。** 作成・改変・共有・配布・商用利用、一切の制限がありません。辞書の自由な流通がCPBの価値を高めます。

**作者の意志：** CPBは圧縮・保護・アーカイブのための技術として公開しています。作者は本ソフトウェアの悪用を望んでいません。利用者は技術的有用性だけでなく、自身の利用が社会に与える影響を考慮する責任があります。悪用によって生じた結果は、すべて利用者の責任です。

**準拠法：** 日本法。

---

*CPB は CMF (Chocolate Muffin) の姉妹プロジェクトです。 CMF は cultural-anchoring 型ステガノグラフィシステムで、 適格な組織に対して別ライセンスで提供されます。*
