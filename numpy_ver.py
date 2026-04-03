import numpy as np
import math
from numba import njit
import time
# ----------------------------
# BMP読み込み（24bit専用）
# ----------------------------
def load_bmp(filename):
    with open(filename, "rb") as f:
        data = f.read()

    if data[0:2] != b"BM":
        raise ValueError("BMPではありません")

    pixel_offset = int.from_bytes(data[10:14], "little")
    width = int.from_bytes(data[18:22], "little", signed=True)
    height = int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    if bpp != 24:
        raise ValueError("24bit BMP専用です")

    bottom_up = True
    if height < 0:
        bottom_up = False
        height = -height

    row_size = ((width * 3 + 3) // 4) * 4
    pixels = []

    for y in range(height):
        row = []
        for x in range(width):
            pos = pixel_offset + y * row_size + x * 3
            b, g, r = data[pos:pos+3]
            row.append((r, g, b))
        pixels.append(row)

    if bottom_up:
        pixels.reverse()

    return np.array(pixels, dtype=np.uint8)

# ----------------------------
# パレット化
# ----------------------------
def make_palette(base):
    # shape: (H, W, 3) → (H*W, 3)
    flat = base.reshape(-1, 3)

    # unique colors
    palette = np.unique(flat, axis=0)

    # 色 → index の辞書
    color_to_index = {}
    for i, color in enumerate(palette):
        color_to_index[tuple(color)] = i

    # index画像を作る
    indexed = np.zeros(base.shape[:2], dtype=np.int32)

    h, w = base.shape[:2]
    for y in range(h):
        for x in range(w):
            indexed[y, x] = color_to_index[tuple(base[y, x])]

    return palette, color_to_index, indexed

# ----------------------------
# 矩形探索
# ----------------------------
def find_next_pixel(used):
    ys, xs = np.where(used == 0)
    if len(xs) == 0:
        return None
    return xs[0], ys[0]

def grow_x_to_y(x, y, base, used):
    c = base[y, x]
    w = 0
    for i in range(x, base.shape[1]):
        if base[y, i] != c or used[y, i] == 1:
            break
        w += 1

    h = 0
    for j in range(y, base.shape[0]):
        if np.any(used[j, x:x+w] == 1) or np.any(base[j, x:x+w] != c):
            break
        h += 1
    return w, h

def grow_y_to_x(x, y, base, used):
    c = base[y, x]
    h = 0
    for j in range(y, base.shape[0]):
        if base[j, x] != c or used[j, x] == 1:
            break
        h += 1

    w = 0
    for i in range(x, base.shape[1]):
        if np.any(used[y:y+h, i] == 1) or np.any(base[y:y+h, i] != c):
            break
        w += 1
    return w, h

def mark_used(x, y, w, h, used):
    used[y:y+h, x:x+w] = 1

# ----------------------------
# 画像エンコード
# ----------------------------
@njit
def encode_image(indexed_base):
    h, w = indexed_base.shape
    used = np.zeros((h, w), dtype=np.uint8)
    rects = []

    curr_y = 0
    curr_x = 0

    while curr_y < h:
        # ----------------------------
        # 次の未使用画素を線形探索
        # ----------------------------
        found = False
        while curr_y < h:
            if used[curr_y, curr_x] == 0:
                found = True
                break

            curr_x += 1
            if curr_x >= w:
                curr_x = 0
                curr_y += 1

        if not found:
            break

        c = indexed_base[curr_y, curr_x]

        # ============================
        # パターン1：横 → 縦
        # ============================
        w1 = 0
        for i in range(curr_x, w):
            if indexed_base[curr_y, i] == c and used[curr_y, i] == 0:
                w1 += 1
            else:
                break

        h1 = 0
        for j in range(curr_y, h):
            ok = True
            for i in range(curr_x, curr_x + w1):
                if indexed_base[j, i] != c or used[j, i] == 1:
                    ok = False
                    break
            if ok:
                h1 += 1
            else:
                break

        # ============================
        # パターン2：縦 → 横
        # ============================
        h2 = 0
        for j in range(curr_y, h):
            if indexed_base[j, curr_x] == c and used[j, curr_x] == 0:
                h2 += 1
            else:
                break

        w2 = 0
        for i in range(curr_x, w):
            ok = True
            for j in range(curr_y, curr_y + h2):
                if indexed_base[j, i] != c or used[j, i] == 1:
                    ok = False
                    break
            if ok:
                w2 += 1
            else:
                break

        # ============================
        # 面積比較
        # ============================
        if w1 * h1 >= w2 * h2:
            final_w = w1
            final_h = h1
        else:
            final_w = w2
            final_h = h2

        # デバッグ（軽め）
        if len(rects) % 5000 == 0:
            print("rects:", len(rects), "pos:", curr_x, curr_y)

        # 保存
        rects.append((final_w, final_h, int(c)))

        # used更新（NumPy使わずループで）
        for j in range(curr_y, curr_y + final_h):
            for i in range(curr_x, curr_x + final_w):
                used[j, i] = 1

    return rects

# ----------------------------
# 指数ゴロム符号
# ----------------------------
def exp_golomb_encode(n):
    b = bin(n)[2:]
    prefix = "0" * (len(b) - 1)
    return prefix + b

def exp_golomb_decode(bits, pos):
    zeros = 0
    while bits[pos] == "0":
        zeros += 1
        pos += 1

    value_bits = bits[pos:pos + zeros + 1]
    pos += zeros + 1
    return int(value_bits, 2), pos

# ----------------------------
# 固定ビット補助
# ----------------------------
def bits_needed(n):
    # 0〜n-1 を表すのに必要なbit数
    if n <= 1:
        return 1
    return math.ceil(math.log2(n))

def int_to_bits(n, width):
    return format(n, f"0{width}b")

# ----------------------------
# パレット + 矩形 → ビット列
# ----------------------------
def encode_rects_to_bits(rects, palette):
    parts = []

    # パレットサイズ
    palette_size = len(palette)
    parts.append(exp_golomb_encode(palette_size))

    # 矩形数
    parts.append(exp_golomb_encode(len(rects)))

    # パレット本体
    for color in palette:
        r, g, b = color
        parts.append(f"{int(r):08b}{int(g):08b}{int(b):08b}")

    # 色インデックスbit数
    color_bits = bits_needed(palette_size)

    # 矩形列
    for w, h, color_index in rects:
        parts.append(exp_golomb_encode(w))
        parts.append(exp_golomb_encode(h))
        parts.append(int_to_bits(color_index, color_bits))

    return "".join(parts)

# ----------------------------
# ビット列 → パレット + 矩形
# ----------------------------
def decode_bits_to_rects(bits, width, height):
    pos = 0

    # パレットサイズ
    palette_size, pos = exp_golomb_decode(bits, pos)

    # 矩形数
    rect_count, pos = exp_golomb_decode(bits, pos)

    # パレット
    palette = []
    for _ in range(palette_size):
        r = int(bits[pos:pos+8], 2); pos += 8
        g = int(bits[pos:pos+8], 2); pos += 8
        b = int(bits[pos:pos+8], 2); pos += 8
        palette.append((r, g, b))

    color_bits = bits_needed(palette_size)

    rects_list = []
    for _ in range(rect_count):
        w, pos = exp_golomb_decode(bits, pos)
        h, pos = exp_golomb_decode(bits, pos)
        c_idx = int(bits[pos:pos+color_bits], 2)
        pos += color_bits
        rects_list.append((w, h, c_idx))

    return np.array(palette, dtype=np.uint8), np.array(rects_list, dtype=np.int32)

# ----------------------------
# デコード
# ----------------------------
@njit
def decode_image(rects_array, palette, width, height):
    canvas = np.zeros((height, width, 3), dtype=np.uint8)
    used = np.zeros((height, width), dtype=np.uint8)
    
    curr_x = 0
    curr_y = 0
    
    for i in range(len(rects_array)):
        rw = rects_array[i, 0]
        rh = rects_array[i, 1]
        ci = rects_array[i, 2]
        
        # --- 次の空きピクセルを探す (Numbaならここが爆速) ---
        while curr_y < height:
            if used[curr_y, curr_x] == 0:
                break
            curr_x += 1
            if curr_x >= width:
                curr_x = 0
                curr_y += 1
        
        if curr_y >= height: break
        
        # --- 描画と使用済みフラグ更新 ---
        color = palette[ci]
        # スライス代入
        canvas[curr_y : curr_y + rh, curr_x : curr_x + rw] = color
        used[curr_y : curr_y + rh, curr_x : curr_x + rw] = 1
        
    return canvas

# ----------------------------
# 実行
# ----------------------------
# ----------------------------
# 実行
# ----------------------------
if __name__ == "__main__":
    filename = r"C:\create_file\vir_project\bmp_tegami_Googled.bmp"

        # 元画像読み込み
    base = load_bmp(filename)
    h, w = base.shape[:2]
    for i in range(2):
        

        print("image size:", w, "x", h)

        # --- エンコード計測開始 ---
        start_encode = time.time()

        # パレット化
        palette, color_to_index, indexed_base = make_palette(base)
        
        # 矩形圧縮
        rects = encode_image(indexed_base)

        # ビット列化
        bits = encode_rects_to_bits(rects, palette)
        
        end_encode = time.time()
        # --- エンコード計測終了 ---

        # --- デコード計測開始 ---
        start_decode = time.time()

        # 復元
        restored_palette, restored_rects = decode_bits_to_rects(bits, w, h)

        restored_palette = np.array(restored_palette, dtype=np.uint8)
        rects_array = np.array(restored_rects, dtype=np.int32)

        decoded = decode_image(rects_array, restored_palette, w, h)
        
        end_decode = time.time()
        # --- デコード計測終了 ---

        # 結果の表示
        print("-" * 30)
        print(f"エンコード時間: {end_encode - start_encode:.4f} 秒")
        print(f"デコード時間:   {end_decode - start_decode:.4f} 秒")
        print("-" * 30)
        print("palette size:", len(palette))
        print("color bits per rect:", bits_needed(len(palette)))
        print("矩形数:", len(rects))
        print("ビット長:", len(bits))
        print("画像復元OK:", np.array_equal(base, decoded))