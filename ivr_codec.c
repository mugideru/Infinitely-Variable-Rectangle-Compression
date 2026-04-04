#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zstd.h>  // zlib.h から差し替え

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// --- データ構造定義 ---

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    uint64_t bit_buffer;
    int bits_in_buffer;
} BitWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
    bool eof; // 追加: ファイル終端検知用フラグ
} BitReader;

typedef struct {
    uint8_t r, g, b;
} Color;

typedef struct {
    uint32_t w, h, c_idx;
} Rect;

typedef struct {
    int width;
    int height;
    Color *pixels;
} Image;

typedef struct {
    uint32_t color; // 0xRRGGBB
    uint32_t index;
    bool occupied;
} HashEntry;

// --- バイナリ操作・補助関数 ---

static inline uint32_t get_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static inline void set_u32_le(uint8_t *data, uint32_t v) {
    data[0] = v & 0xFF;
    data[1] = (v >> 8) & 0xFF;
    data[2] = (v >> 16) & 0xFF;
    data[3] = (v >> 24) & 0xFF;
}

static int bits_needed(uint32_t n) {
    if (n <= 1) return 1;
    return (int)ceil(log2((double)n));
}

// --- ビット操作関連 (BitWriter / BitReader) ---

static void bw_init(BitWriter *bw) {
    bw->capacity = 1024 * 1024;
    bw->data = (uint8_t *)malloc(bw->capacity);
    bw->size = 0;
    bw->bit_buffer = 0;
    bw->bits_in_buffer = 0;
}

static void bw_write_bits(BitWriter *bw, uint32_t value, int width) {
    uint64_t mask = (width == 64) ? ~0ULL : (1ULL << width) - 1;
    bw->bit_buffer = (bw->bit_buffer << width) | (value & mask);
    bw->bits_in_buffer += width;
    while (bw->bits_in_buffer >= 8) {
        if (bw->size >= bw->capacity) {
            bw->capacity *= 2;
            bw->data = (uint8_t *)realloc(bw->data, bw->capacity);
        }
        bw->data[bw->size++] = (uint8_t)(bw->bit_buffer >> (bw->bits_in_buffer - 8));
        bw->bits_in_buffer -= 8;
    }
}

static void bw_write_exp_golomb(BitWriter *bw, uint32_t n) {
    int bits = 0;
    uint32_t temp = n;
    while (temp > 0) {
        bits++;
        temp >>= 1;
    }
    if (bits > 1) {
        bw_write_bits(bw, 0, bits - 1);
    }
    bw_write_bits(bw, n, bits);
}

static void bw_finish(BitWriter *bw) {
    if (bw->bits_in_buffer > 0) {
        bw_write_bits(bw, 0, 8 - bw->bits_in_buffer);
    }
}

static void br_init(BitReader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->eof = false;
}

static uint32_t br_read_bits(BitReader *br, int width) {
    uint32_t val = 0;
    for (int i = 0; i < width; i++) {
        if (br->byte_pos >= br->size) {
            br->eof = true;
            return 0;
        }
        val = (val << 1) | ((br->data[br->byte_pos] >> (7 - br->bit_pos)) & 1);
        if (++br->bit_pos == 8) {
            br->bit_pos = 0;
            br->byte_pos++;
        }
    }
    return val;
}

static uint32_t br_read_exp_golomb(BitReader *br) {
    int zeros = 0;
    while (br_read_bits(br, 1) == 0) {
        if (br->eof) return 0; // 無限ループ回避
        zeros++;
    }
    uint32_t val = 1;
    for (int i = 0; i < zeros; i++) {
        val = (val << 1) | br_read_bits(br, 1);
    }
    return val;
}

// --- BMP読み書き関連 ---

static void write_bmp_stream(FILE *stream, Image img) {
    if (!img.pixels || !stream) return;
    int row_size = ((img.width * 3 + 3) / 4) * 4;
    
    // Strict Aliasingを回避しつつヘッダを構築
    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    set_u32_le(&header[2], 54 + row_size * img.height);
    set_u32_le(&header[10], 54);
    set_u32_le(&header[14], 40);
    set_u32_le(&header[18], img.width);
    set_u32_le(&header[22], img.height);
    header[26] = 1;  // planes
    header[28] = 24; // bpp
    
    fwrite(header, 1, 54, stream);
    uint8_t *row = (uint8_t *)calloc(1, row_size);
    for (int y = img.height - 1; y >= 0; y--) {
        for (int x = 0; x < img.width; x++) {
            Color c = img.pixels[y * img.width + x];
            row[x*3+0] = c.b; 
            row[x*3+1] = c.g; 
            row[x*3+2] = c.r;
        }
        fwrite(row, 1, row_size, stream);
    }
    free(row);
}

static void save_bmp(const char *filename, Image img) {
    FILE *f = fopen(filename, "wb");
    if (f) { 
        write_bmp_stream(f, img); 
        fclose(f); 
    }
}

static Image load_bmp(const char *filename) {
    Image img = {0, 0, NULL};
    FILE *f = fopen(filename, "rb");
    if (!f) return img;
    
    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54) {
        fclose(f);
        return img;
    }
    
    img.width = (int32_t)get_u32_le(&h[18]);
    int32_t h_img = (int32_t)get_u32_le(&h[22]);
    img.height = abs(h_img);
    img.pixels = (Color*)malloc((size_t)img.width * img.height * sizeof(Color));
    
    int row_size = ((img.width * 3 + 3) / 4) * 4;
    uint8_t *row = (uint8_t*)malloc(row_size);
    
    for (int y = 0; y < img.height; y++) {
        if (fread(row, 1, row_size, f) != row_size) break;
        int dy = (h_img > 0) ? (img.height - 1 - y) : y;
        for (int x = 0; x < img.width; x++) {
            img.pixels[dy * img.width + x] = (Color){row[x*3+2], row[x*3+1], row[x*3+0]};
        }
    }
    free(row);
    fclose(f);
    return img;
}

// --- zlib ユーティリティ ---

static uint8_t* zstd_compress_optimized(const uint8_t *src, size_t src_len, size_t *out_len) {
    size_t const cap = ZSTD_compressBound(src_len);
    uint8_t *dest = (uint8_t *)malloc(cap);
    if (!dest) return NULL;

    // 圧縮レベル 19 は高圧縮ですが、デコード速度は速いまま維持されます
    size_t const c_size = ZSTD_compress(dest, cap, src, src_len, 19);
    
    if (ZSTD_isError(c_size)) {
        free(dest);
        return NULL;
    }
    *out_len = c_size;
    return dest;
}

static uint8_t* zstd_decompress_optimized(const uint8_t *src, size_t src_len, size_t expected_sz) {
    uint8_t *dest = (uint8_t *)malloc(expected_sz);
    if (!dest) return NULL;

    size_t const d_size = ZSTD_decompress(dest, expected_sz, src, src_len);
    
    if (ZSTD_isError(d_size)) {
        free(dest);
        return NULL;
    }
    return dest;
}

// --- エンコード処理関連 ---

static inline uint32_t hash_func(uint32_t color) {
    color = ((color >> 16) ^ color) * 0x45d9f3b;
    color = ((color >> 16) ^ color) * 0x45d9f3b;
    color = (color >> 16) ^ color;
    return color; 
}

static void resize_hash_table(HashEntry **table, uint32_t *current_size, uint32_t pal_cnt) {
    uint32_t old_size = *current_size;
    uint32_t new_size = old_size * 2;
    HashEntry *old_table = *table;
    
    HashEntry *new_table = (HashEntry *)calloc(new_size, sizeof(HashEntry));
    uint32_t mask = new_size - 1;

    for (uint32_t i = 0; i < old_size; i++) {
        if (old_table[i].occupied) {
            uint32_t c = old_table[i].color;
            uint32_t h = hash_func(c) & mask; 
            while (new_table[h].occupied) {
                h = (h + 1) & mask;
            }
            new_table[h] = old_table[i];
        }
    }

    free(old_table);
    *table = new_table;
    *current_size = new_size;
}

static void make_palette(Image img, Color **out_palette, uint32_t *out_pal_size, uint32_t **out_indexed) {
    int total_pixels = img.width * img.height;
    uint32_t current_hash_size = 65536;
    HashEntry *table = (HashEntry *)calloc(current_hash_size, sizeof(HashEntry));
    
    uint32_t palette_cap = 65536;
    Color *palette = (Color *)malloc(palette_cap * sizeof(Color)); 
    uint32_t *indexed = (uint32_t *)malloc(total_pixels * sizeof(uint32_t));
    uint32_t pal_cnt = 0;

    for (int i = 0; i < total_pixels; i++) {
        uint32_t c = (img.pixels[i].r << 16) | (img.pixels[i].g << 8) | img.pixels[i].b;
        uint32_t mask = current_hash_size - 1;
        uint32_t h = hash_func(c) & mask;

        while (table[h].occupied) {
            if (table[h].color == c) break;
            h = (h + 1) & mask;
        }

        if (!table[h].occupied) {
            // floatの計算を整数計算に変更
            if (pal_cnt * 10 > current_hash_size * 7) {
                resize_hash_table(&table, &current_hash_size, pal_cnt);
                mask = current_hash_size - 1;
                h = hash_func(c) & mask;
                while (table[h].occupied) {
                    h = (h + 1) & mask;
                }
            }

            if (pal_cnt >= palette_cap) {
                palette_cap *= 2;
                palette = (Color *)realloc(palette, palette_cap * sizeof(Color));
            }

            table[h].occupied = true;
            table[h].color = c;
            table[h].index = pal_cnt;

            palette[pal_cnt].r = (c >> 16) & 0xFF;
            palette[pal_cnt].g = (c >> 8) & 0xFF;
            palette[pal_cnt].b = c & 0xFF;

            indexed[i] = pal_cnt;
            pal_cnt++;
        } else {
            indexed[i] = table[h].index;
        }
    }
    free(table);

    *out_palette = (Color *)realloc(palette, pal_cnt * sizeof(Color));
    *out_pal_size = pal_cnt;
    *out_indexed = indexed;
}

static Rect* encode_image(uint32_t *indexed, int w, int h, uint32_t *out_rect_count) {
    uint8_t *used = (uint8_t *)calloc((size_t)w * h, sizeof(uint8_t));
    size_t rect_cap = 10000;
    Rect *rects = (Rect *)malloc(rect_cap * sizeof(Rect));
    uint32_t rect_count = 0;
    int curr_y = 0, curr_x = 0;

    while (curr_y < h) {
        bool found = false;
        while (curr_y < h) {
            if (used[curr_y * w + curr_x] == 0) {
                found = true;
                break;
            }
            curr_x++;
            if (curr_x >= w) {
                curr_x = 0;
                curr_y++;
            }
        }
        if (!found) break;

        uint32_t c = indexed[curr_y * w + curr_x];

        // 横→縦の広がりを計算
        int w1 = 0;
        for (int i = curr_x; i < w; i++) {
            if (indexed[curr_y * w + i] == c && used[curr_y * w + i] == 0) w1++;
            else break;
        }
        int h1 = 0;
        for (int j = curr_y; j < h; j++) {
            bool ok = true;
            for (int i = curr_x; i < curr_x + w1; i++) {
                if (indexed[j * w + i] != c || used[j * w + i] == 1) {
                    ok = false; break;
                }
            }
            if (ok) h1++; else break;
        }

        // 縦→横の広がりを計算
        int h2 = 0;
        for (int j = curr_y; j < h; j++) {
            if (indexed[j * w + curr_x] == c && used[j * w + curr_x] == 0) h2++;
            else break;
        }
        int w2 = 0;
        for (int i = curr_x; i < w; i++) {
            bool ok = true;
            for (int j = curr_y; j < curr_y + h2; j++) {
                if (indexed[j * w + i] != c || used[j * w + i] == 1) {
                    ok = false; break;
                }
            }
            if (ok) w2++; else break;
        }

        // より面積の広い矩形を採用
        int final_w = (w1 * h1 >= w2 * h2) ? w1 : w2;
        int final_h = (w1 * h1 >= w2 * h2) ? h1 : h2;

        if (rect_count >= rect_cap) {
            rect_cap *= 2;
            rects = (Rect *)realloc(rects, rect_cap * sizeof(Rect));
        }
        rects[rect_count].w = final_w;
        rects[rect_count].h = final_h;
        rects[rect_count].c_idx = c;
        rect_count++;

        for (int j = curr_y; j < curr_y + final_h; j++) {
            for (int i = curr_x; i < curr_x + final_w; i++) {
                used[j * w + i] = 1;
            }
        }
    }

    free(used);
    *out_rect_count = rect_count;
    return rects;
}

static Image decode_image(Rect *rects, uint32_t rect_count, Color *palette, int w, int h, int scale_x, int scale_y) {
    Image img;
    img.width = w * scale_x;
    img.height = h * scale_y;
    img.pixels = (Color *)malloc((size_t)img.width * img.height * sizeof(Color));

    uint8_t *used = (uint8_t *)calloc((size_t)w * h, sizeof(uint8_t));
    int curr_x = 0, curr_y = 0;

    for (uint32_t k = 0; k < rect_count; k++) {
        uint32_t rw = rects[k].w;
        uint32_t rh = rects[k].h;
        uint32_t ci = rects[k].c_idx;

        // 元画像上の次の未使用位置を探す
        while (curr_y < h) {
            if (used[curr_y * w + curr_x] == 0) break;
            curr_x++;
            if (curr_x >= w) {
                curr_x = 0;
                curr_y++;
            }
        }
        if (curr_y >= h) break;

        Color color = palette[ci];

        // 元画像上で使用済みにする
        for (int j = curr_y; j < curr_y + (int)rh; j++) {
            for (int i = curr_x; i < curr_x + (int)rw; i++) {
                used[j * w + i] = 1;
            }
        }

        // 拡大後画像へ直接描画
        int dx0 = curr_x * scale_x;
        int dy0 = curr_y * scale_y;
        int dx1 = (curr_x + (int)rw) * scale_x;
        int dy1 = (curr_y + (int)rh) * scale_y;

        for (int y = dy0; y < dy1; y++) {
            for (int x = dx0; x < dx1; x++) {
                img.pixels[y * img.width + x] = color;
            }
        }
    }

    free(used);
    return img;
}

// --- IVR ファイル I/O ---

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t buf[4];
    set_u32_le(buf, v);
    fwrite(buf, 1, 4, f);
}

static uint32_t read_u32_le(FILE *f) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return get_u32_le(buf);
}

static void bmp_to_ivr(const char *bmp_filename, const char *ivr_filename) {
    fprintf(stderr, "=== BMP -> IVR ===\n\n");
    clock_t t_start = clock();

    Image img = load_bmp(bmp_filename);
    if (!img.pixels) return;
    fprintf(stderr, "image size: %d x %d\n\n", img.width, img.height);

    clock_t t1 = clock();
    Color *palette;
    uint32_t pal_size;
    uint32_t *indexed;
    make_palette(img, &palette, &pal_size, &indexed);
    clock_t t2 = clock();
    fprintf(stderr, "パレット作成時間: %.4f 秒\n\n", (double)(t2 - t1) / CLOCKS_PER_SEC);

    clock_t t3 = clock();
    uint32_t rect_count;
    Rect *rects = encode_image(indexed, img.width, img.height, &rect_count);
    clock_t t4 = clock();
    fprintf(stderr, "エンコード時間:   %.4f 秒\n\n", (double)(t4 - t3) / CLOCKS_PER_SEC);

    BitWriter bw;
    bw_init(&bw);
    bw_write_exp_golomb(&bw, pal_size);
    bw_write_exp_golomb(&bw, rect_count);

    Color last_c = {0, 0, 0};
    for (uint32_t i = 0; i < pal_size; i++) {
        bw_write_bits(&bw, (uint8_t)(palette[i].r - last_c.r), 8);
        bw_write_bits(&bw, (uint8_t)(palette[i].g - last_c.g), 8);
        bw_write_bits(&bw, (uint8_t)(palette[i].b - last_c.b), 8);
        last_c = palette[i]; 
    }

    int color_bits = bits_needed(pal_size);
    for (uint32_t i = 0; i < rect_count; i++) {
        bw_write_exp_golomb(&bw, rects[i].w);
        bw_write_exp_golomb(&bw, rects[i].h);
        bw_write_bits(&bw, rects[i].c_idx, color_bits);
    }
    bw_finish(&bw);

    size_t compressed_size;
    uint8_t *compressed = zstd_compress_optimized(bw.data, bw.size, &compressed_size);

    clock_t t5 = clock();
    FILE *out = fopen(ivr_filename, "wb");
    if (out) {
        fwrite("IVR1", 1, 4, out);      
        write_u32_le(out, img.width);
        write_u32_le(out, img.height);
        write_u32_le(out, (uint32_t)bw.size);
        fwrite(compressed, 1, compressed_size, out);
        fclose(out);
    }
    clock_t t6 = clock();
    fprintf(stderr, "保存時間:         %.4f 秒\n", (double)(t6 - t5) / CLOCKS_PER_SEC);

    fprintf(stderr, "\npalette size: %u\n", pal_size);
    fprintf(stderr, "color bits per rect: %d\n", color_bits);
    fprintf(stderr, "矩形数: %u\n", rect_count);
    fprintf(stderr, "raw bytes: %zu\n", bw.size);
    fprintf(stderr, "Zstd bytes: %zu\n\n", compressed_size);

    free(img.pixels); free(palette); free(indexed); free(rects);
    free(bw.data); free(compressed);
}

static Image ivr_to_image(const char *ivr_in, int sx, int sy) {
    fprintf(stderr, "=== IVR -> Decoding ===\n\n");
    clock_t t0 = clock();

    FILE *f = fopen(ivr_in, "rb");
    if (!f) return (Image){0, 0, NULL};
    
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "IVR1", 4) != 0) {
        fprintf(stderr, "エラー: Zstd形式のIVRファイルではありません\n");
        fclose(f); return (Image){0, 0, NULL};
    }
    
    uint32_t w = read_u32_le(f);
    uint32_t h = read_u32_le(f);
    uint32_t raw_size = read_u32_le(f);
    
    long current_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t z_sz = (size_t)ftell(f) - current_pos;
    fseek(f, current_pos, SEEK_SET);
    
    uint8_t *z_buf = (uint8_t *)malloc(z_sz);
    fread(z_buf, 1, z_sz, f);
    fclose(f);
    
    uint8_t *raw = zstd_decompress_optimized(z_buf, z_sz, raw_size);
    free(z_buf);
    
    if (!raw) return (Image){0, 0, NULL};

    BitReader br; 
    br_init(&br, raw, raw_size);
    uint32_t pal_sz = br_read_exp_golomb(&br);
    uint32_t rect_cnt = br_read_exp_golomb(&br);
    
    Color *pal = (Color *)malloc(pal_sz * sizeof(Color));
    Color cur = {0, 0, 0};
    for (uint32_t i = 0; i < pal_sz; i++) {
        cur.r += (uint8_t)br_read_bits(&br, 8);
        cur.g += (uint8_t)br_read_bits(&br, 8);
        cur.b += (uint8_t)br_read_bits(&br, 8);
        pal[i] = cur;
    }
    
    int c_bits = bits_needed(pal_sz);
    Rect *rects = (Rect *)malloc(rect_cnt * sizeof(Rect));
    for (uint32_t i = 0; i < rect_cnt; i++) {
        rects[i].w = br_read_exp_golomb(&br);
        rects[i].h = br_read_exp_golomb(&br);
        rects[i].c_idx = br_read_bits(&br, c_bits);
    }
    
    Image img = decode_image(rects, rect_cnt, pal, w, h, sx, sy);
    clock_t t1 = clock();

    fprintf(stderr, "デコード時間: %.4f 秒\n", (double)(t1 - t0) / CLOCKS_PER_SEC);
    fprintf(stderr, "展開後サイズ: %d x %d (Scale: %dx%d)\n\n", img.width, img.height, sx, sy);
    
    free(raw); free(pal); free(rects);
    return img;
}

// ivr_to_bmp はメイン関数で使われていませんでしたが、ツール用途として残しています
static void ivr_to_bmp(const char *ivr_filename, const char *bmp_filename, int scale_x, int scale_y) {
    clock_t t0 = clock();
    
    Image img = ivr_to_image(ivr_filename, scale_x, scale_y);
    if (!img.pixels) return;
    clock_t t1 = clock();

    save_bmp(bmp_filename, img);
    clock_t t2 = clock();

    fprintf(stderr, "展開時間: %.4f 秒\n", (double)(t1 - t0) / CLOCKS_PER_SEC);
    fprintf(stderr, "BMP保存時間: %.4f 秒\n", (double)(t2 - t1) / CLOCKS_PER_SEC);
    fprintf(stderr, "復元完了: %s\n", bmp_filename);

    free(img.pixels);
}

// --- メイン関数 ---

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <in.bmp> <out.ivr> <scale_x> <scale_y>\n", argv[0]);
        return 1;
    }
    
    const char *bmp_in = argv[1];
    const char *ivr_file = argv[2];
    int sx = atoi(argv[3]);
    int sy = atoi(argv[4]);

    // 1. IVR作成 (エンコード)
    bmp_to_ivr(bmp_in, ivr_file);

    // 2. IVR読み込みと展開 (デコード)
    Image preview = ivr_to_image(ivr_file, sx, sy);
    if (!preview.pixels) return 1;

    // 3. 標準出力へBMPとして流す
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    write_bmp_stream(stdout, preview);

    free(preview.pixels);
    return 0;
}