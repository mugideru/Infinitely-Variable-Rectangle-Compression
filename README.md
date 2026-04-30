
-----

# IVR (Infinitely Variable Rectangle , Imitate Vector Rendering)

-----

## English

IVR is a **high-speed, lossless image compression format** that represents images as a sequence of optimized, same-color rectangles. It occupies a unique niche between traditional raster formats (like PNG) and vector-like structured data.

### 🚀 Performance at a Glance

  - **High-Speed Decoding:** Reaches up to **350+ MP/s**, making it ideal for real-time applications.
  - **Superior Compression:** Outperforms PNG by **10–60%** on structured, flat-colored, or UI-heavy images.
  - **Native Scalability:** Can decode at arbitrary scales (e.g., 2x, 4x) by simply multiplying rectangle dimensions, avoiding heavy pixel-interpolation overhead.

### 🎯 Best Use Cases

  - **UI & Web Graphics:** Buttons, icons, and wireframes.
  - **Pixel Art & Sprites:** Perfect for game assets where pixel-perfect precision is required.
  - **Document Archiving:** Scanned text, forms, and diagrams.
  - **Runtime Caching:** Fast intermediate storage for engine textures or screenshots.

### 🛠 Repository Contents

  - `ivr.py`: Reference implementation in Python (focus on readability).
  - `ivr.c`: High-performance C implementation (focus on speed).
  - `zstd` integration for advanced entropy coding.

-----

## Technical Overview

### Core Concept: "Rectangle Scanning"

Unlike PNG which scans line-by-line, IVR identifies the **largest possible same-color rectangle** starting from the top-left unprocessed pixel. It compares horizontal-first vs. vertical-first growth and selects the one with the maximum area.

### Format Specification (IVR1)

1.  **Header:** Magic bytes (`IVR1`), Width, Height.
2.  **Payload (Zstd compressed):**
      - **Palette:** Adaptive size with delta-encoding for RGB values.
      - **Rectangle List:** - `Width`: Exp-Golomb encoded.
          - `Height`: Exp-Golomb encoded.
          - `Color Index`: Fixed-bit length based on palette size.



**(Benchmark)**

**Image:  2559 x 1391 (VSCODE)**

  - **BMP:** 10.1 MB
  - **PNG:** 268 KB
  - **IVR:** **92.9 KB** (Down to approx. 33% of PNG size)

  - **Encode Time:** **0.0105s**
  - **Zstd Time:** **0.0283s**
  - **Decode Time:** **0.0144s**

<img width="2559" height="1391" alt="スクリーンショット 2026-05-01 053137" src="https://github.com/user-attachments/assets/03e77e9f-492a-4b69-934d-2ec61aa486cc" />

Log:
=== File Info ===
Input Size : 274440 bytes

=== Any -> IVR ===
Resolution : 2559 x 1391 (Raw)
Scale      : x1 , y1
Output Res : 2559 x 1391

Palette    : 4082 colors
Rectangles : 72837
Raw Data   : 152524 bytes
Zstd Data  : 92973 bytes
Ratio      : 33.88 % of original
Zstd Level : 19

Make Pal   : 0.0075 s
Encode     : 0.0105 s
Zip        : 0.0283 s
Decode     : 0.0144 s
TOTAL      : 0.0715 s

-----
### Security & Safety
Since IVR can represent massive images with tiny file sizes (similar to a "zip bomb"), implementations include safety guards for:

  - Max output dimensions/pixel count.
  - Recursion/Allocation limits.

-----

## 日本語

IVR (Infinitely Variable Rectangle) は、画像を「最適化された同色長方形の集合」として表現する、**超高速・可逆圧縮画像フォーマット**です。PNGのような従来のラスタ形式と、構造化データの利点を併せ持っています。

### 🚀 パフォーマンスの核心

  - **爆速デコード:** 最大 **350 MP/s** 以上の処理速度を誇り、4K画像も一瞬で展開可能です。
  - **高い圧縮効率:** UIグラフィックスやフラットイラストにおいて、PNG比で **10%〜60%** のサイズ削減を実現します。
  - **ネイティブ・スケーリング:** 矩形データとして記録されているため、デコード時に座標を倍数化するだけで、計算負荷を抑えたまま巨大な解像度へ拡大展開（Scale）が可能です。

### 🎯 最適な用途

  - **UI・ウェブグラフィックス:** ボタン、アイコン、ワイヤーフレームなど。
  - **ドット絵・スプライト:** ゲーム制作における精密な資産管理。
  - **文書・図面アーカイブ:** スキャンされた書類、FAX、回路図。
  - **ランタイムキャッシュ:** ゲームエンジン内での一時的なテクスチャ保存やスクショ。

-----

## 技術的な仕組み

### 1\. 矩形探索アルゴリズム

IVRは左上の未処理ピクセルを起点に、\*\*「最も面積が大きくなる同色の長方形」\*\*を探索します。

  - 「横方向に伸ばしてから縦に広げる」
  - 「縦方向に伸ばしてから横に広げる」
    この2パターンを比較し、より効率的な方を採用することで、PNGの走査線方式（Filter）よりも構造的に冗長性を排除します。

### 2\. データ構造 (IVR1)

Zstdによる強力なエントロピー圧縮の前に、データを以下の構造に最適化します。

  - **パレット:** RGBの差分（Delta）をとることで連続性を向上。
  - **矩形リスト:** - `幅 / 高さ`: 指数ゴロム符号（Exp-Golomb）による可変長符号化。
      - `色インデックス`: パレット数に応じた最小ビット割り当て。

-----

## 実測値の例 (Benchmark)

**Image:  2559 x 1391 (VSCODE)**

  - **BMP:** 10.1 MB
  - **PNG:** 268 KB
  - **IVR:** **92.9 KB** (PNG比 約33%)

  - **Encode Time:** **0.0105s**
  - **Zstd Time:** **0.0283s**
  - **Decode Time:** **0.0144s**

<img width="2559" height="1391" alt="スクリーンショット 2026-05-01 053137" src="https://github.com/user-attachments/assets/03e77e9f-492a-4b69-934d-2ec61aa486cc" />

Log:
=== File Info ===
Input Size : 274440 bytes

=== Any -> IVR ===
Resolution : 2559 x 1391 (Raw)
Scale      : x1 , y1
Output Res : 2559 x 1391

Palette    : 4082 colors
Rectangles : 72837
Raw Data   : 152524 bytes
Zstd Data  : 92973 bytes
Ratio      : 33.88 % of original
Zstd Level : 19

Make Pal   : 0.0075 s
Encode     : 0.0105 s
Zip        : 0.0283 s
Decode     : 0.0144 s
TOTAL      : 0.0715 s

-----
### ⚠️ セキュリティ上の注意

IVRは「小さなファイルから巨大な画素」を生成できるため、信頼できないファイルを扱う際は以下の制限を設けています。

  - 最大出力画素数および拡大率の制限
  - メモリ確保前のオーバーフロー・チェック

### ライセンス

MIT License

-----
