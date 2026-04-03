# IVR (Indexed Variable Rectangles)

# English

IVR is a **lossless image compression format that represents an image as a sequence of same-color rectangles**.

It works especially well for:

- low-color images
- mosaic-like images
- pixel art
- flat illustrations
- UI-like graphics

This repository includes:

- a **Python implementation**
- a **C implementation (fast version)**
- BMP ↔ IVR conversion tools
- a simple format specification

---

## Features

- **Lossless compression**
- Relatively simple implementation
- Very fast decoding
- Extremely effective on flat / low-color images
- High compression when combined with zlib

---

## Core Idea

Instead of storing pixels directly, IVR tries to describe an image as:

> **a sequence of maximum same-color rectangles scanned from top-left to bottom-right**

For example, this image:

```text
AAAAAA
AAAAAA
BBBBCC
BBBBCC
```

can be represented roughly as:

- one large rectangle of A
- one rectangle of B
- one rectangle of C

Each rectangle is stored as:

- palette index
- width
- height

Then the whole stream is compressed with **zlib**.

---

## File Format (IVR1)

Current simple format:

### File Header

```text
[4 bytes] Magic: "IVR1"
[4 bytes] Width  (little endian)
[4 bytes] Height (little endian)
[rest]     zlib-compressed payload
```

### Payload (after zlib decompression)

```text
Exp-Golomb: palette_size
Exp-Golomb: rect_count

Palette:
  palette_size × (R:8bit, G:8bit, B:8bit)

Rectangles:
  rect_count × (
    width  : Exp-Golomb
    height : Exp-Golomb
    color  : fixed bits (depends on palette size)
  )
```

---

## Algorithm Overview

### 1. Palette creation
All unique colors are collected and the image is converted into an indexed image.

### 2. Rectangle extraction
Starting from the top-left unprocessed pixel, IVR tests:

- **horizontal → vertical growth**
- **vertical → horizontal growth**

Then it chooses the rectangle with the larger area.

### 3. Bitstream encoding
Each rectangle is stored as:

- width (Exp-Golomb)
- height (Exp-Golomb)
- color index (fixed bit length)

### 4. zlib compression
The final bitstream is compressed with zlib.

---

## Decoding

Decoding is intentionally simple:

1. Read palette
2. Read rectangle list
3. Repaint rectangles in scan order

Because of this, **decoding can be extremely fast**.

---

## Example Result

Example test image: **1700 × 2200**

```text
palette size: 47
rect count: 60292
raw bytes: 75659
zlib bytes: 46452
BMP size: 11220054 bytes
IVR size: 46468 bytes
compression ratio vs BMP: 0.00414
```

That means:

- **BMP: ~11.2 MB**
- **IVR: ~46 KB**

This is, of course, a very IVR-friendly image — but it shows the potential clearly.

---

## Compared to PNG

IVR is **not meant to replace PNG universally**.  
It has a very specific sweet spot.

### Strong cases for IVR
- low-color images
- mosaic images
- pixel art
- UI graphics
- flat-color illustrations
- region-structured images

### Weak cases for IVR
- photographs
- noisy images
- gradients
- highly textured content

So IVR is best understood as:

> **a lossless codec specialized for images that are easy to explain with rectangles**

---

## Implementations

### Python version
- good for experimentation
- easy to read and modify

### C version
- much faster
- suitable for performance testing
- significantly faster than Python

---

## Future Ideas

- better rectangle search strategies
- improved row-tracking structures
- additional entropy coding
- predictive coding
- GPU decoding
- formal format specification

---

## Disclaimer

IVR is currently an **experimental format**.  
Compatibility and archival stability are not guaranteed yet.

---

## License

MIT License

**日本語**

IVR は、**画像を「同色の長方形」の列として表現する可逆画像圧縮フォーマット**です。  
特に、**少色画像・モザイク画像・ドット絵・UI風画像・フラットなイラスト**に対して非常に高い圧縮率を発揮します。

このリポジトリには、以下が含まれます：

- **Python 実装**
- **C 実装（高速版）**
- BMP ↔ IVR 変換ツール
- フォーマット仕様（簡易）

---

## 特徴

- **可逆圧縮**（完全復元）
- **実装が比較的シンプル**
- **デコードが非常に速い**
- 少色・大面積単色画像で非常に強い
- zlib と組み合わせることで高圧縮

---

## 圧縮の考え方

通常の画像圧縮は「ピクセル列」や「周波数」を扱いますが、IVR はもっと単純です。

> **画像を、左上から順番に「同じ色で塗れる最大長方形」に分割する**

たとえば、こういう画像：

```text
AAAAAA
AAAAAA
BBBBCC
BBBBCC
```

は、おおむね以下のような矩形列に分解されます：

- A の大きな長方形
- B の長方形
- C の長方形

これを

- **パレット**
- **長方形の幅**
- **長方形の高さ**
- **色インデックス**

として保存します。

その後、全体を **zlib** で圧縮します。

---

## フォーマット概要（IVR1）

現在の簡易フォーマットは以下のようになっています：

### ファイルヘッダ

```text
[4 bytes] Magic: "IVR1"
[4 bytes] Width  (little endian)
[4 bytes] Height (little endian)
[rest]     zlib-compressed payload
```

### ペイロード（zlib展開後）

```text
Exp-Golomb: palette_size
Exp-Golomb: rect_count

Palette:
  palette_size × (R:8bit, G:8bit, B:8bit)

Rectangles:
  rect_count × (
    width  : Exp-Golomb
    height : Exp-Golomb
    color  : fixed bits (depends on palette size)
  )
```

---

## アルゴリズム概要

### 1. パレット化
画像中の色をユニーク化し、RGB → インデックス画像へ変換します。

### 2. 矩形分割
左上から順に、未処理ピクセルを起点に：

- **横→縦**
- **縦→横**

の2パターンで最大長方形を求め、  
**面積の大きい方**を採用します。

### 3. ビット列化
各矩形を以下で記録します：

- 幅（Exp-Golomb）
- 高さ（Exp-Golomb）
- 色インデックス（固定bit）

### 4. zlib圧縮
最後に zlib でまとめて圧縮します。

---

## 復号（デコード）

IVR の復元は非常に単純です。

1. パレットを読む
2. 矩形列を読む
3. 左上から順番に長方形を塗る

この設計により、**デコードはかなり高速**です。

---

## 実測例

例：1700 × 2200 のテスト画像

```text
palette size: 47
rect count: 60292
raw bytes: 75659
zlib bytes: 46452
BMP size: 11220054 bytes
IVR size: 46468 bytes
compression ratio vs BMP: 0.00414
```

つまり：

- **BMP: 約 11.2 MB**
- **IVR: 約 46 KB**

かなり極端な圧縮が可能です。

> ※ もちろん、これは IVR に非常に向いた画像での例です。

---

## PNG との関係

IVR は **PNG の代替を常に狙うものではありません**。  
向いている画像がかなりはっきりしています。

### IVR が強い画像
- 少色画像
- モザイク画像
- ドット絵
- UI風画像
- ベタ塗りイラスト
- 領域分割しやすい画像

### IVR が弱い画像
- 写真
- ノイズが多い画像
- グラデーションが多い画像
- テクスチャ密度が高い画像

つまり IVR は、

> **「矩形で説明しやすい画像」に特化した可逆圧縮方式**

です。

---

## 実装

### Python 版
- 実験・検証向け
- アルゴリズムの確認に便利

### C 版
- 高速版
- 実用テスト向け
- Python より大幅に高速

---

## 今後やりたいこと

- より良い矩形探索戦略
- 行ごとの管理構造の改善
- 追加のエントロピー圧縮
- 予測器との併用
- GPU デコード
- 正式なフォーマット仕様書

---

## 注意

これは現在 **実験的なフォーマット**です。  
互換性や長期保存用途はまだ考慮していません。

---

## ライセンス

MIT License

---

