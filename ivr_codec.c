#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zstd.h>  
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
    bool failed;
} BitWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
    bool eof; 
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

// --- 補助マクロ/関数 ---
static inline bool check_mul_overflow(size_t a, size_t b, size_t *res) {
    if (a > 0 && b > SIZE_MAX / a) return true;
    *res = a * b;
    return false;
}

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
    int bits = 0;
    n--;
    while (n > 0) {
        bits++;
        n >>= 1;
    }
    return bits;
}

// --- ビット操作関連 ---

static bool bw_init(BitWriter *bw) {
    bw->capacity = 1024 * 1024;
    bw->data = (uint8_t *)malloc(bw->capacity);
    bw->size = 0;
    bw->bit_buffer = 0;
    bw->bits_in_buffer = 0;
    bw->failed = (bw->data == NULL);
    return !bw->failed;
}
static void bw_write_bits(BitWriter *bw, uint32_t value, int width) {
    if (!bw->data || bw->failed) return;
    if (width < 0 || width > 32) { bw->failed = true; return; }

    while (bw->bits_in_buffer + width > 56) { // 64でもいいが余裕を持たせる
        if (bw->size >= bw->capacity) {
            if (bw->capacity > SIZE_MAX / 2) { bw->failed = true; return; }
            size_t new_cap = bw->capacity * 2;
            uint8_t *new_data = (uint8_t *)realloc(bw->data, new_cap);
            if (!new_data) { bw->failed = true; return; }
            bw->data = new_data;
            bw->capacity = new_cap;
        }
        bw->data[bw->size++] = (uint8_t)(bw->bit_buffer >> (bw->bits_in_buffer - 8));
        bw->bits_in_buffer -= 8;
    }

    uint64_t mask = (width == 0) ? 0 : ((1ULL << width) - 1);
    bw->bit_buffer = (bw->bit_buffer << width) | ((uint64_t)value & mask);
    bw->bits_in_buffer += width;

    while (bw->bits_in_buffer >= 8) {
        if (bw->size >= bw->capacity) {
            if (bw->capacity > SIZE_MAX / 2) { bw->failed = true; return; }
            size_t new_cap = bw->capacity * 2;
            uint8_t *new_data = (uint8_t *)realloc(bw->data, new_cap);
            if (!new_data) { bw->failed = true; return; }
            bw->data = new_data;
            bw->capacity = new_cap;
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
    if (bits > 1) bw_write_bits(bw, 0, bits - 1);
    bw_write_bits(bw, n, bits);
}

static void bw_finish(BitWriter *bw) {
    if (!bw || bw->failed) return;
    if (bw->bits_in_buffer < 0 || bw->bits_in_buffer >= 64) {
        bw->failed = true;
        return;
    }
    if (bw->bits_in_buffer > 0) {
        int pad = (8 - (bw->bits_in_buffer % 8)) % 8;
        if (pad) bw_write_bits(bw, 0, pad);
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
    while (br_read_bits(br, 1) == 0 && !br->eof) {
        zeros++;
        if (zeros > 32) { br->eof = true; return 0; } // 異常データ防御
    }
    if (br->eof) return 0;
    uint32_t val = 1;
    for (int i = 0; i < zeros; i++) {
        val = (val << 1) | br_read_bits(br, 1);
    }
    return val;
}

// --- BMP読み書き関連 ---

static void write_bmp_stream(FILE *stream, Image img) {
    if (!img.pixels || !stream || img.width <= 0 || img.height <= 0) return;
    
    long long row_size_tmp = ((img.width * 3LL + 3) / 4) * 4;
    if (row_size_tmp > INT_MAX) return;
    int row_size = (int)row_size_tmp;

    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    set_u32_le(&header[2], 54 + (uint32_t)row_size * img.height);
    set_u32_le(&header[10], 54);
    set_u32_le(&header[14], 40);
    set_u32_le(&header[18], (uint32_t)img.width);
    set_u32_le(&header[22], (uint32_t)img.height);
    header[26] = 1; header[28] = 24; 
    
    fwrite(header, 1, 54, stream);
    uint8_t *row = (uint8_t *)calloc(1, row_size);
    if (!row) return;

    for (int y = img.height - 1; y >= 0; y--) {
        for (int x = 0; x < img.width; x++) {
            Color c = img.pixels[y * img.width + x];
            row[x*3+0] = c.b; row[x*3+1] = c.g; row[x*3+2] = c.r;
        }
        fwrite(row, 1, row_size, stream);
    }
    free(row);
}

static void save_bmp(const char *filename, Image img) {
    FILE *f = fopen(filename, "wb");
    if (f) { write_bmp_stream(f, img); fclose(f); }
}

static Image load_bmp(const char *filename) {
    Image img = {0, 0, NULL};
    FILE *f = fopen(filename, "rb");
    if (!f) return img;
    
    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54) { fclose(f); return img; }
    if (h[0] != 'B' || h[1] != 'M') { fclose(f); return img; }

    uint32_t dib_size = get_u32_le(&h[14]);
    uint16_t planes = h[26] | (h[27] << 8);
    uint16_t bpp = h[28] | (h[29] << 8);
    uint32_t compression = get_u32_le(&h[30]);
    uint32_t pixel_offset = get_u32_le(&h[10]);

    if (dib_size < 40 || planes != 1 || bpp != 24 || compression != 0 || pixel_offset < 54) {
        fclose(f);
        return img;
    }
    if (fseek(f, pixel_offset, SEEK_SET) != 0) {
        fclose(f);
        return img;
    }
    img.width = (int32_t)get_u32_le(&h[18]);
    int32_t h_img = (int32_t)get_u32_le(&h[22]);
    img.height = abs(h_img);
    if (img.width <= 0 || img.height <= 0 || img.width > 32768 || img.height > 32768) { fclose(f); return img; }

    size_t alloc_sz;
    if (check_mul_overflow((size_t)img.width * img.height, sizeof(Color), &alloc_sz)) { fclose(f); return img; }
    img.pixels = (Color*)malloc(alloc_sz);
    if (!img.pixels) { fclose(f); return img; }
    
    int row_size = ((img.width * 3 + 3) / 4) * 4;
    uint8_t *row = (uint8_t*)malloc(row_size);
    if (!row) { free(img.pixels); img.pixels = NULL; fclose(f); return img; }
    
    for (int y = 0; y < img.height; y++) {
        if (fread(row, 1, row_size, f) != (size_t)row_size) break;
        int dy = (h_img > 0) ? (img.height - 1 - y) : y;
        for (int x = 0; x < img.width; x++) {
            img.pixels[dy * img.width + x] = (Color){row[x*3+2], row[x*3+1], row[x*3+0]};
        }
    }
    free(row); fclose(f);
    return img;
}

// --- Zstd ユーティリティ ---

static uint8_t* zstd_compress_optimized(const uint8_t *src, size_t src_len, size_t *out_len) {
    if (src_len == 0) return NULL;
    size_t const cap = ZSTD_compressBound(src_len);
    uint8_t *dest = (uint8_t *)malloc(cap);
    if (!dest) return NULL;
    size_t const c_size = ZSTD_compress(dest, cap, src, src_len, 19);
    if (ZSTD_isError(c_size)) { free(dest); return NULL; }
    *out_len = c_size;
    return dest;
}

static uint8_t* zstd_decompress_optimized(const uint8_t *src, size_t src_len, size_t expected_sz) {
    if (expected_sz == 0 || expected_sz > 1024 * 1024 * 1024) return NULL; // 最大1024MB制限
    uint8_t *dest = (uint8_t *)malloc(expected_sz);
    if (!dest) return NULL;
    size_t const d_size = ZSTD_decompress(dest, expected_sz, src, src_len);
    if (ZSTD_isError(d_size) || d_size != expected_sz) { free(dest); return NULL; }
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
    if (!new_table) return;

    uint32_t mask = new_size - 1;
    for (uint32_t i = 0; i < old_size; i++) {
        if (old_table[i].occupied) {
            uint32_t c = old_table[i].color;
            uint32_t h = hash_func(c) & mask; 
            while (new_table[h].occupied) h = (h + 1) & mask;
            new_table[h] = old_table[i];
        }
    }
    free(old_table);
    *table = new_table;
    *current_size = new_size;
}

static void make_palette(Image img, Color **out_palette, uint32_t *out_pal_size, uint32_t **out_indexed) {
    size_t total_pixels = (size_t)img.width * img.height;
    uint32_t current_hash_size = 65536;
    HashEntry *table = (HashEntry *)calloc(current_hash_size, sizeof(HashEntry));
    
    uint32_t palette_cap = 65536;
    Color *palette = (Color *)malloc(palette_cap * sizeof(Color)); 
    uint32_t *indexed = (uint32_t *)malloc(total_pixels * sizeof(uint32_t));
    uint32_t pal_cnt = 0;

    if (!table || !palette || !indexed) {
        free(table);
        free(palette);
        free(indexed);
        *out_palette = NULL;
        *out_pal_size = 0;
        *out_indexed = NULL;
        return;
    }

    for (size_t i = 0; i < total_pixels; i++) {
        uint32_t c = (img.pixels[i].r << 16) | (img.pixels[i].g << 8) | img.pixels[i].b;
        uint32_t mask = current_hash_size - 1;
        uint32_t h = hash_func(c) & mask;

        while (table[h].occupied) {
            if (table[h].color == c) break;
            h = (h + 1) & mask;
        }

        if (!table[h].occupied) {
            if (pal_cnt * 10 > current_hash_size * 7) {
                resize_hash_table(&table, &current_hash_size, pal_cnt);
                mask = current_hash_size - 1;
                h = hash_func(c) & mask;
                while (table[h].occupied) h = (h + 1) & mask;
            }
            if (pal_cnt >= palette_cap) {
                if (palette_cap > UINT32_MAX / 2) {
                    free(table); free(palette); free(indexed);
                    *out_palette = NULL; *out_pal_size = 0; *out_indexed = NULL;
                    return;
                }

                uint32_t new_cap = palette_cap * 2;
                Color *new_pal = (Color *)realloc(palette, (size_t)new_cap * sizeof(Color));
                if (!new_pal) {
                    free(table); free(palette); free(indexed);
                    *out_palette = NULL; *out_pal_size = 0; *out_indexed = NULL;
                    return;
                }
                palette = new_pal;
                palette_cap = new_cap;
            }
            table[h].occupied = true;
            table[h].color = c;
            table[h].index = pal_cnt;
            palette[pal_cnt] = (Color){(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
            indexed[i] = pal_cnt++;
        } else {
            indexed[i] = table[h].index;
        }
    }
    free(table);
    *out_palette = palette;
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
            if (used[curr_y * w + curr_x] == 0) { found = true; break; }
            if (++curr_x >= w) { curr_x = 0; curr_y++; }
        }
        if (!found) break;

        uint32_t c = indexed[curr_y * w + curr_x];
        int w1 = 0;
        for (int i = curr_x; i < w && indexed[curr_y * w + i] == c && !used[curr_y * w + i]; i++) w1++;
        int h1 = 0;
        for (int j = curr_y; j < h; j++) {
            bool ok = true;
            for (int i = curr_x; i < curr_x + w1; i++) 
                if (indexed[j * w + i] != c || used[j * w + i]) { ok = false; break; }
            if (ok) h1++; else break;
        }
        int h2 = 0;
        for (int j = curr_y; j < h && indexed[j * w + curr_x] == c && !used[j * w + curr_x]; j++) h2++;
        int w2 = 0;
        for (int i = curr_x; i < w; i++) {
            bool ok = true;
            for (int j = curr_y; j < curr_y + h2; j++)
                if (indexed[j * w + i] != c || used[j * w + i]) { ok = false; break; }
            if (ok) w2++; else break;
        }
        int final_w = (w1 * h1 >= w2 * h2) ? w1 : w2;
        int final_h = (w1 * h1 >= w2 * h2) ? h1 : h2;

        if (rect_count >= rect_cap) {
            if (rect_cap > SIZE_MAX / 2 / sizeof(Rect)) {
                free(used);
                free(rects);
                *out_rect_count = 0;
                return NULL;
            }
            size_t new_cap = rect_cap * 2;
            Rect *new_rects = (Rect *)realloc(rects, new_cap * sizeof(Rect));
            if (!new_rects) {
                free(used);
                free(rects);
                *out_rect_count = 0;
                return NULL;
            }
            rects = new_rects;
            rect_cap = new_cap;
        }
        rects[rect_count++] = (Rect){(uint32_t)final_w, (uint32_t)final_h, c};
        for (int j = curr_y; j < curr_y + final_h; j++)
            for (int i = curr_x; i < curr_x + final_w; i++) used[j * w + i] = 1;
    }
    free(used);
    *out_rect_count = rect_count;
    return rects;
}

static Image decode_image(Rect *rects, uint32_t rect_count, Color *palette, int w, int h, int scale_x, int scale_y, uint32_t pal_sz) {
    Image img = {0, 0, NULL};
    if (w <= 0 || h <= 0 || scale_x <= 0 || scale_y <= 0) return img;
    
    size_t total_p;
    if (check_mul_overflow((size_t)w * scale_x, (size_t)h * scale_y, &total_p)) return img;
    if (check_mul_overflow(total_p, sizeof(Color), &total_p)) return img;

    img.width = w * scale_x;
    img.height = h * scale_y;
    img.pixels = (Color *)calloc(1, total_p);
    uint8_t *used = (uint8_t *)calloc(1, (size_t)w * h);
    if (!img.pixels || !used) { free(img.pixels); free(used); return (Image){0,0,NULL}; }

    int curr_x = 0, curr_y = 0;
    for (uint32_t k = 0; k < rect_count; k++) {
        while (curr_y < h && used[curr_y * w + curr_x]) {
            if (++curr_x >= w) { curr_x = 0; curr_y++; }
        }
        if (curr_y >= h) break;

        uint32_t rw = rects[k].w, rh = rects[k].h, ci = rects[k].c_idx;
        if (ci >= pal_sz || curr_x + (int)rw > w || curr_y + (int)rh > h) continue;

        Color color = palette[ci];
        for (int j = curr_y; j < curr_y + (int)rh; j++)
            for (int i = curr_x; i < curr_x + (int)rw; i++) used[j * w + i] = 1;

        int dx0 = curr_x * scale_x, dy0 = curr_y * scale_y;
        int dx1 = (curr_x + (int)rw) * scale_x, dy1 = (curr_y + (int)rh) * scale_y;
        for (int y = dy0; y < dy1; y++)
            for (int x = dx0; x < dx1; x++) img.pixels[y * img.width + x] = color;
    }
    free(used);
    return img;
}

// --- IVR ファイル I/O ---

static void write_u32_le(FILE *f, uint32_t v) {
    uint8_t buf[4]; set_u32_le(buf, v); fwrite(buf, 1, 4, f);
}

static uint32_t read_u32_le(FILE *f) {
    uint8_t buf[4]; return (fread(buf, 1, 4, f) == 4) ? get_u32_le(buf) : 0;
}

static void bmp_to_ivr(const char *bmp_filename, const char *ivr_filename) {
    fprintf(stderr, "=== BMP -> IVR ===\n\n");
    Image img = load_bmp(bmp_filename);
    if (!img.pixels) return;
    fprintf(stderr, "image size: %d x %d\n\n", img.width, img.height);

    clock_t t1 = clock();
    Color *palette; uint32_t pal_size; uint32_t *indexed;
    make_palette(img, &palette, &pal_size, &indexed);
    if (!palette || !indexed) {
        free(img.pixels);
        return;
    }
    fprintf(stderr, "パレット作成時間: %.4f 秒\n\n", (double)(clock() - t1) / CLOCKS_PER_SEC);

    clock_t t3 = clock();
    uint32_t rect_count;
    Rect *rects = encode_image(indexed, img.width, img.height, &rect_count);
    fprintf(stderr, "エンコード時間:   %.4f 秒\n\n", (double)(clock() - t3) / CLOCKS_PER_SEC);

    BitWriter bw;
    if (!bw_init(&bw)) return;
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
    for (uint32_t i = 0; i < rect_count; i++) bw_write_exp_golomb(&bw, rects[i].w);
    for (uint32_t i = 0; i < rect_count; i++) bw_write_exp_golomb(&bw, rects[i].h);
    for (uint32_t i = 0; i < rect_count; i++) bw_write_bits(&bw, rects[i].c_idx, color_bits);
    bw_finish(&bw);

    size_t compressed_size;
    uint8_t *compressed = zstd_compress_optimized(bw.data, bw.size, &compressed_size);
    if (compressed) {
        FILE *out = fopen(ivr_filename, "wb");
        if (out) {
            fwrite("IVR1", 1, 4, out);
            write_u32_le(out, (uint32_t)img.width);
            write_u32_le(out, (uint32_t)img.height);
            write_u32_le(out, (uint32_t)bw.size);
            fwrite(compressed, 1, compressed_size, out);
            fclose(out);
        }
        free(compressed);
    }
    fprintf(stderr, "\npalette size: %u\n矩形数: %u\nZstd bytes: %zu\n\n", pal_size, rect_count, compressed_size);
    free(img.pixels); free(palette); free(indexed); free(rects); free(bw.data);
}

static Image ivr_to_image(const char *ivr_in, int sx, int sy) {
    fprintf(stderr, "=== IVR -> Decoding ===\n\n");
    clock_t t0 = clock();
    FILE *f = fopen(ivr_in, "rb");
    if (!f) return (Image){0, 0, NULL};
    
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "IVR1", 4) != 0) {
        fprintf(stderr, "エラー: 無効な形式\n"); fclose(f); return (Image){0, 0, NULL};
    }
    
    uint32_t w = read_u32_le(f), h = read_u32_le(f), raw_size = read_u32_le(f);
    long current_pos = ftell(f);
    if (current_pos < 0) { fclose(f); return (Image){0,0,NULL}; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return (Image){0,0,NULL}; }
    long end_pos = ftell(f);
    if (end_pos < 0 || end_pos < current_pos) { fclose(f); return (Image){0,0,NULL}; }

    size_t z_sz = (size_t)(end_pos - current_pos);
    if (fseek(f, current_pos, SEEK_SET) != 0) { fclose(f); return (Image){0,0,NULL}; }
    
    uint8_t *z_buf = (uint8_t *)malloc(z_sz);
    if (!z_buf) { fclose(f); return (Image){0, 0, NULL}; }
    if (fread(z_buf, 1, z_sz, f) != z_sz) {
        free(z_buf);
        fclose(f);
        return (Image){0,0,NULL};
    }
    fclose(f);


    uint8_t *raw = zstd_decompress_optimized(z_buf, z_sz, raw_size);
    free(z_buf);
    if (!raw) return (Image){0, 0, NULL};

    BitReader br; br_init(&br, raw, raw_size);
    uint32_t pal_sz = br_read_exp_golomb(&br);
    uint32_t rect_cnt = br_read_exp_golomb(&br);
    if (pal_sz > 16000000 || rect_cnt > 16000000) { free(raw); return (Image){0, 0, NULL}; }

    Color *pal = (Color *)malloc(pal_sz * sizeof(Color));
    if (!pal) { free(raw); return (Image){0, 0, NULL}; }
    Color cur = {0, 0, 0};
    for (uint32_t i = 0; i < pal_sz; i++) {
        cur.r += (uint8_t)br_read_bits(&br, 8); cur.g += (uint8_t)br_read_bits(&br, 8); cur.b += (uint8_t)br_read_bits(&br, 8);
        pal[i] = cur;
    }
    
    int c_bits = bits_needed(pal_sz);
    Rect *rects = (Rect *)malloc(rect_cnt * sizeof(Rect));
    if (!rects) { free(raw); free(pal); return (Image){0, 0, NULL}; }

    for (uint32_t i = 0; i < rect_cnt; i++) rects[i].w = br_read_exp_golomb(&br);
    for (uint32_t i = 0; i < rect_cnt; i++) rects[i].h = br_read_exp_golomb(&br);
    for (uint32_t i = 0; i < rect_cnt; i++) rects[i].c_idx = br_read_bits(&br, c_bits);

    Image img = decode_image(rects, rect_cnt, pal, (int)w, (int)h, sx, sy, pal_sz);
    fprintf(stderr, "デコード時間: %.4f 秒\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    free(raw); free(pal); free(rects);
    return img;
}

static void ivr_to_bmp(const char *ivr_filename, const char *bmp_filename, int scale_x, int scale_y) {
    Image img = ivr_to_image(ivr_filename, scale_x, scale_y);
    if (img.pixels) { save_bmp(bmp_filename, img); free(img.pixels); }
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <in.bmp> <out.ivr> <scale_x> <scale_y>\n", argv[0]);
        return 1;
    }
    int sx = atoi(argv[3]), sy = atoi(argv[4]);
    if (sx <= 0 || sy <= 0) return 1;

    bmp_to_ivr(argv[1], argv[2]);
    Image preview = ivr_to_image(argv[2], sx, sy);
    if (!preview.pixels) return 1;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    write_bmp_stream(stdout, preview);
    free(preview.pixels);
    return 0;
}