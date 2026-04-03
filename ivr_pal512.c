#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zlib.h>





typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    uint64_t bit_buffer; // ビットを一時的に貯める「貯金箱」
    int bits_in_buffer;  // 貯金箱に今何ビット入っているか
} BitWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
} BitReader;

typedef struct {
    uint8_t r, g, b;
} Color;

typedef struct {
    uint32_t w, h;
    uint32_t color;      // 0xRRGGBB
    uint16_t pal_idx;    // 0..511
    uint8_t is_palette;  // 1=palette, 0=raw RGB
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

typedef struct {
    uint32_t color;
    uint32_t count;
} ColorCount;

int find_palette_index(Color *palette, uint32_t pal_size, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (uint32_t i = 0; i < pal_size; i++) {
        if (palette[i].r == r && palette[i].g == g && palette[i].b == b) {
            return (int)i;
        }
    }
    return -1;
}

// BitWriter / BitReader
void bw_init(BitWriter *bw) {
    bw->capacity = 1024 * 1024; // 1MB
    bw->data = (uint8_t *)malloc(bw->capacity);
    bw->size = 0;
    bw->bit_buffer = 0;
    bw->bits_in_buffer = 0;
}

// 複数をまとめて書く（メインの処理）
void bw_write_bits(BitWriter *bw, uint32_t value, int width) {
    // 貯金箱に新しいビットを入れる
    uint64_t mask = (width == 64) ? ~0ULL : (1ULL << width) - 1;
    bw->bit_buffer = (bw->bit_buffer << width) | (value & mask);
    bw->bits_in_buffer += width;

    // 8ビット以上溜まったら、1バイトずつメモリへ書き出す
    while (bw->bits_in_buffer >= 8) {
        if (bw->size >= bw->capacity) {
            bw->capacity *= 2;
            bw->data = (uint8_t *)realloc(bw->data, bw->capacity);
        }
        bw->data[bw->size++] = (uint8_t)(bw->bit_buffer >> (bw->bits_in_buffer - 8));
        bw->bits_in_buffer -= 8;
    }
}

// 1ビットだけ書く（互換性用）
void bw_write_bit(BitWriter *bw, uint8_t bit) {
    bw_write_bits(bw, bit, 1);
}

// 指数ゴロムも新しい bw_write_bits を使って爆速化
void bw_write_exp_golomb(BitWriter *bw, uint32_t n) {
    if (n < 1) exit(1);
    int bits = 0;
    uint32_t temp = n;
    while (temp > 0) { bits++; temp >>= 1; }
    
    if (bits > 1) {
        bw_write_bits(bw, 0, bits - 1); // 0の並びを一気書き
    }
    bw_write_bits(bw, n, bits); // 本体のビットを一気書き
}

void bw_finish(BitWriter *bw) {
    // 残りカス（8ビットに満たない分）を左詰めで出力
    if (bw->bits_in_buffer > 0) {
        uint8_t last_byte = (uint8_t)(bw->bit_buffer << (8 - bw->bits_in_buffer));
        if (bw->size >= bw->capacity) {
            bw->data = (uint8_t *)realloc(bw->data, bw->size + 1);
        }
        bw->data[bw->size++] = last_byte;
        bw->bits_in_buffer = 0;
        bw->bit_buffer = 0;
    }
}

void br_init(BitReader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

uint8_t br_read_bit(BitReader *br) {
    if (br->byte_pos >= br->size) {
        fprintf(stderr, "ビット列終端です\n");
        exit(1);
    }
    uint8_t byte = br->data[br->byte_pos];
    uint8_t bit = (byte >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

uint32_t br_read_bits(BitReader *br, int width) {
    uint32_t value = 0;
    for (int i = 0; i < width; i++) {
        value = (value << 1) | br_read_bit(br);
    }
    return value;
}

uint32_t br_read_exp_golomb(BitReader *br) {
    int zeros = 0;
    while (br_read_bit(br) == 0) zeros++;
    uint32_t value = 1;
    for (int i = 0; i < zeros; i++) {
        value = (value << 1) | br_read_bit(br);
    }
    return value;
}

// 補助関数
int bits_needed(uint32_t n) {
    if (n <= 1) return 1;
    return (int)ceil(log2((double)n));
}

int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t*)a;
    uint32_t y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

// BMP読み書き（24bit専用）
Image load_bmp(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "BMPを開けません: %s\n", filename); exit(1); }

    uint8_t header[54];
    fread(header, 1, 54, f);
    if (header[0] != 'B' || header[1] != 'M') {
        fprintf(stderr, "BMPではありません\n"); exit(1);
    }

    uint32_t pixel_offset = *(uint32_t*)&header[10];
    int32_t width = *(int32_t*)&header[18];
    int32_t height = *(int32_t*)&header[22];
    uint16_t bpp = *(uint16_t*)&header[28];

    if (bpp != 24) {
        fprintf(stderr, "24bit BMP専用です\n"); exit(1);
    }

    bool bottom_up = true;
    if (height < 0) {
        bottom_up = false;
        height = -height;
    }

    int row_size = ((width * 3 + 3) / 4) * 4;
    fseek(f, pixel_offset, SEEK_SET);

    uint8_t *raw_row = (uint8_t *)malloc(row_size);
    Image img;
    img.width = width;
    img.height = height;
    img.pixels = (Color *)malloc(width * height * sizeof(Color));

    for (int y = 0; y < height; y++) {
        fread(raw_row, 1, row_size, f);
        int dest_y = bottom_up ? (height - 1 - y) : y;
        for (int x = 0; x < width; x++) {
            // BMPはBGRで記録されているのでRGBに変換して保持
            img.pixels[dest_y * width + x].b = raw_row[x * 3 + 0];
            img.pixels[dest_y * width + x].g = raw_row[x * 3 + 1];
            img.pixels[dest_y * width + x].r = raw_row[x * 3 + 2];
        }
    }
    free(raw_row);
    fclose(f);
    return img;
}

void save_bmp(const char *filename, Image img) {
    FILE *f = fopen(filename, "wb");
    int row_size = ((img.width * 3 + 3) / 4) * 4;
    uint32_t pixel_array_size = row_size * img.height;
    uint32_t file_size = 54 + pixel_array_size;

    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    *(uint32_t*)&header[2] = file_size;
    *(uint32_t*)&header[10] = 54;
    *(uint32_t*)&header[14] = 40;
    *(int32_t*)&header[18] = img.width;
    *(int32_t*)&header[22] = img.height; // 正ならBottom-Up
    *(uint16_t*)&header[26] = 1;
    *(uint16_t*)&header[28] = 24;
    *(uint32_t*)&header[34] = pixel_array_size;
    *(uint32_t*)&header[38] = 2835;
    *(uint32_t*)&header[42] = 2835;

    fwrite(header, 1, 54, f);

    uint8_t *row_buf = (uint8_t *)calloc(1, row_size);
    for (int y = img.height - 1; y >= 0; y--) { // 下から上へ
        for (int x = 0; x < img.width; x++) {
            Color c = img.pixels[y * img.width + x];
            row_buf[x * 3 + 0] = c.b;
            row_buf[x * 3 + 1] = c.g;
            row_buf[x * 3 + 2] = c.r;
        }
        fwrite(row_buf, 1, row_size, f);
    }
    free(row_buf);
    fclose(f);
}

// zlib ユーティリティ
uint8_t* zlib_compress_level9(const uint8_t *src, size_t src_len, size_t *out_len) {
    z_stream strm = {0};
    if (deflateInit(&strm, 9) != Z_OK) return NULL;
    
    size_t cap = deflateBound(&strm, src_len);
    uint8_t *dest = (uint8_t *)malloc(cap);
    
    strm.next_in = (z_const Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dest;
    strm.avail_out = (uInt)cap;
    
    deflate(&strm, Z_FINISH);
    *out_len = strm.total_out;
    deflateEnd(&strm);
    return dest;
}

uint8_t* zlib_decompress_alloc(const uint8_t *src, size_t src_len, size_t *out_len) {
    size_t cap = src_len * 4 + 8192;
    uint8_t *dest = (uint8_t *)malloc(cap);
    z_stream strm = {0};
    strm.next_in = (z_const Bytef *)src;
    strm.avail_in = (uInt)src_len;
    
    if (inflateInit(&strm) != Z_OK) { free(dest); return NULL; }

    do {
        strm.next_out = dest + strm.total_out;
        strm.avail_out = (uInt)(cap - strm.total_out);
        int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&strm); free(dest); return NULL;
        }
        if (strm.avail_out == 0) {
            cap *= 2;
            dest = (uint8_t *)realloc(dest, cap);
        }
    } while (1);

    *out_len = strm.total_out;
    inflateEnd(&strm);
    return dest;
}
inline uint32_t hash_func(uint32_t color) {
    color = ((color >> 16) ^ color) * 0x45d9f3b;
    color = ((color >> 16) ^ color) * 0x45d9f3b;
    color = (color >> 16) ^ color;
    return color; 
}

void resize_hash_table(HashEntry **table, uint32_t *current_size, uint32_t pal_cnt) {
    uint32_t old_size = *current_size;
    uint32_t new_size = old_size * 2;
    HashEntry *old_table = *table;
    
    // 新しいサイズのテーブルを確保し、0で初期化
    HashEntry *new_table = (HashEntry *)calloc(new_size, sizeof(HashEntry));
    uint32_t mask = new_size - 1;

    // 古いテーブルのデータを新しいテーブルへ再配置（リハッシュ）
    for (uint32_t i = 0; i < old_size; i++) {
        if (old_table[i].occupied) {
            uint32_t c = old_table[i].color;
            uint32_t h = hash_func(c) & mask; // 新しいサイズに合わせたハッシュ値
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

int cmp_color_count_desc(const void *a, const void *b) {
    const ColorCount *x = (const ColorCount*)a;
    const ColorCount *y = (const ColorCount*)b;
    if (x->count < y->count) return 1;
    if (x->count > y->count) return -1;
    return 0;
}

void make_top_palette(Image img, Color **out_palette, uint32_t *out_pal_size) {
    int total_pixels = img.width * img.height;

    uint32_t hash_size = 65536;
    HashEntry *table = (HashEntry *)calloc(hash_size, sizeof(HashEntry));
    ColorCount *counts = (ColorCount *)malloc(65536 * sizeof(ColorCount));
    uint32_t count_cap = 65536;
    uint32_t unique_cnt = 0;

    for (int i = 0; i < total_pixels; i++) {
        uint32_t c = (img.pixels[i].r << 16) | (img.pixels[i].g << 8) | img.pixels[i].b;
        uint32_t mask = hash_size - 1;
        uint32_t h = hash_func(c) & mask;

        while (table[h].occupied) {
            if (table[h].color == c) break;
            h = (h + 1) & mask;
        }

        if (!table[h].occupied) {
            if (unique_cnt > hash_size * 0.7) {
                resize_hash_table(&table, &hash_size, unique_cnt);
                mask = hash_size - 1;
                h = hash_func(c) & mask;
                while (table[h].occupied) {
                    h = (h + 1) & mask;
                }
            }

            if (unique_cnt >= count_cap) {
                count_cap *= 2;
                counts = (ColorCount *)realloc(counts, count_cap * sizeof(ColorCount));
            }

            table[h].occupied = true;
            table[h].color = c;
            table[h].index = unique_cnt;

            counts[unique_cnt].color = c;
            counts[unique_cnt].count = 1;
            unique_cnt++;
        } else {
            counts[table[h].index].count++;
        }
    }

    qsort(counts, unique_cnt, sizeof(ColorCount), cmp_color_count_desc);

    uint32_t pal_size = unique_cnt < 512 ? unique_cnt : 512;
    Color *palette = (Color *)malloc(pal_size * sizeof(Color));

    for (uint32_t i = 0; i < pal_size; i++) {
        uint32_t c = counts[i].color;
        palette[i].r = (c >> 16) & 0xFF;
        palette[i].g = (c >> 8) & 0xFF;
        palette[i].b = c & 0xFF;
    }

    free(table);
    free(counts);

    *out_palette = palette;
    *out_pal_size = pal_size;
}

// パレット化・矩形エンコード・デコード
void make_palette(Image img, Color **out_palette, uint32_t *out_pal_size, uint32_t **out_indexed) {
    int total_pixels = img.width * img.height;

    // 上位512色だけパレット化
    make_top_palette(img, out_palette, out_pal_size);

    // indexed には「パレット index」ではなく「生のRGB値」を入れる
    uint32_t *indexed = (uint32_t *)malloc(total_pixels * sizeof(uint32_t));

    for (int i = 0; i < total_pixels; i++) {
        indexed[i] = (img.pixels[i].r << 16) | (img.pixels[i].g << 8) | img.pixels[i].b;
    }

    *out_indexed = indexed;
}
Rect* encode_image(uint32_t *indexed, int w, int h, uint32_t *out_rect_count) {
    uint8_t *used = (uint8_t *)calloc(w * h, sizeof(uint8_t));
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

        // 横→縦
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

        // 縦→横
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

        int final_w = (w1 * h1 >= w2 * h2) ? w1 : w2;
        int final_h = (w1 * h1 >= w2 * h2) ? h1 : h2;

        if (rect_count >= rect_cap) {
            rect_cap *= 2;
            rects = (Rect *)realloc(rects, rect_cap * sizeof(Rect));
        }

        rects[rect_count].w = final_w;
        rects[rect_count].h = final_h;
        rects[rect_count].color = c;
        rects[rect_count].is_palette = 0;
        rects[rect_count].pal_idx = 0;
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

Image decode_image(Rect *rects, uint32_t rect_count, Color *palette,
                   int w, int h, int scale_x, int scale_y) {
    Image img;
    img.width = w * scale_x;
    img.height = h * scale_y;
    img.pixels = (Color *)malloc(img.width * img.height * sizeof(Color));

    uint8_t *used = (uint8_t *)calloc(w * h, sizeof(uint8_t));

    int curr_x = 0, curr_y = 0;

    for (uint32_t k = 0; k < rect_count; k++) {
        uint32_t rw = rects[k].w;
        uint32_t rh = rects[k].h;

        while (curr_y < h) {
            if (used[curr_y * w + curr_x] == 0) break;
            curr_x++;
            if (curr_x >= w) {
                curr_x = 0;
                curr_y++;
            }
        }
        if (curr_y >= h) break;

        uint32_t c = rects[k].color;
        Color color;
        color.r = (c >> 16) & 0xFF;
        color.g = (c >> 8) & 0xFF;
        color.b = c & 0xFF;

        for (int j = curr_y; j < curr_y + (int)rh; j++) {
            for (int i = curr_x; i < curr_x + (int)rw; i++) {
                used[j * w + i] = 1;
            }
        }

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

// IVR ファイル I/O
void write_u32_le(FILE *f, uint32_t v) {
    uint8_t buf[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    fwrite(buf, 1, 4, f);
}

uint32_t read_u32_le(FILE *f) {
    uint8_t buf[4];
    fread(buf, 1, 4, f);
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

void bmp_to_ivr(const char *bmp_filename, const char *ivr_filename) {
    printf("=== BMP -> IVR ===\n");
    Image img = load_bmp(bmp_filename);
    printf("image size: %d x %d\n", img.width, img.height);

    clock_t t0 = clock();
    Color *palette;
    uint32_t pal_size;
    uint32_t *indexed;
    make_palette(img, &palette, &pal_size, &indexed);
    clock_t t1 = clock();

    uint32_t rect_count;
    Rect *rects = encode_image(indexed, img.width, img.height, &rect_count);

    BitWriter bw;
    bw_init(&bw);
    bw_write_exp_golomb(&bw, pal_size);
    bw_write_exp_golomb(&bw, rect_count);

    for (uint32_t i = 0; i < pal_size; i++) {
        bw_write_bits(&bw, palette[i].r, 8);
        bw_write_bits(&bw, palette[i].g, 8);
        bw_write_bits(&bw, palette[i].b, 8);
    }

    int color_bits = bits_needed(pal_size);

    for (uint32_t i = 0; i < rect_count; i++) {
        int pidx = find_palette_index(palette, pal_size, rects[i].color);

        bw_write_exp_golomb(&bw, rects[i].w);
        bw_write_exp_golomb(&bw, rects[i].h);

        if (pidx >= 0) {
            bw_write_bit(&bw, 1);                  // palette color
            bw_write_bits(&bw, (uint32_t)pidx, color_bits);
        } else {
            bw_write_bit(&bw, 0);                  // raw RGB
            bw_write_bits(&bw, rects[i].color, 24);
        }
    }
    bw_finish(&bw);

    size_t compressed_size;
    uint8_t *compressed = zlib_compress_level9(bw.data, bw.size, &compressed_size);
    clock_t t2 = clock();

    FILE *out = fopen(ivr_filename, "wb");
    fwrite("IVR1", 1, 4, out);
    write_u32_le(out, img.width);
    write_u32_le(out, img.height);
    fwrite(compressed, 1, compressed_size, out);
    fclose(out);
    clock_t t3 = clock();

    printf("パレット作成時間: %.4f 秒\n", (double)(t1 - t0) / CLOCKS_PER_SEC);
    printf("エンコード時間:   %.4f 秒\n", (double)(t2 - t1) / CLOCKS_PER_SEC);
    printf("保存時間:         %.4f 秒\n", (double)(t3 - t2) / CLOCKS_PER_SEC);
    printf("----------------------------------------\n");
    printf("palette size: %u (top colors only)\n", pal_size);
    printf("palette index bits: %d\n", color_bits);
    printf("矩形数: %u\n", rect_count);
    printf("raw bytes: %zu\n", bw.size);
    printf("zlib bytes: %zu\n", compressed_size);
    printf("----------------------------------------\n");

    free(img.pixels);
    free(palette);
    free(indexed);
    free(rects);
    free(bw.data);
    free(compressed);
}

void ivr_to_bmp(const char *ivr_filename, const char *bmp_filename) {
    printf("=== IVR -> BMP ===\n");
    clock_t t0 = clock();

    FILE *f = fopen(ivr_filename, "rb");
    if (!f) { fprintf(stderr, "IVRを開けません: %s\n", ivr_filename); exit(1); }
    
    uint8_t magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, "IVR1", 4) != 0) {
        fprintf(stderr, "IVRファイルではありません\n"); exit(1);
    }

    int width = read_u32_le(f);
    int height = read_u32_le(f);

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    size_t compressed_size = file_size - 12;
    fseek(f, 12, SEEK_SET);

    uint8_t *compressed = (uint8_t *)malloc(compressed_size);
    fread(compressed, 1, compressed_size, f);
    fclose(f);

    size_t raw_size;
    uint8_t *raw_data = zlib_decompress_alloc(compressed, compressed_size, &raw_size);
    free(compressed);

    BitReader br;
    br_init(&br, raw_data, raw_size);

    uint32_t pal_size = br_read_exp_golomb(&br);
    uint32_t rect_count = br_read_exp_golomb(&br);

    Color *palette = (Color *)malloc(pal_size * sizeof(Color));
    for (uint32_t i = 0; i < pal_size; i++) {
        palette[i].r = br_read_bits(&br, 8);
        palette[i].g = br_read_bits(&br, 8);
        palette[i].b = br_read_bits(&br, 8);
    }

    int color_bits = bits_needed(pal_size);
    Rect *rects = (Rect *)malloc(rect_count * sizeof(Rect));

    for (uint32_t i = 0; i < rect_count; i++) {
        rects[i].w = br_read_exp_golomb(&br);
        rects[i].h = br_read_exp_golomb(&br);

        rects[i].is_palette = br_read_bit(&br);

        if (rects[i].is_palette) {
            rects[i].pal_idx = (uint16_t)br_read_bits(&br, color_bits);
            rects[i].color =
                (palette[rects[i].pal_idx].r << 16) |
                (palette[rects[i].pal_idx].g << 8) |
                (palette[rects[i].pal_idx].b);
        } else {
            rects[i].color = br_read_bits(&br, 24);
            rects[i].pal_idx = 0;
        }
    }
    int decode_x_scale = 1; // This is not a scallop, it's a scaling.
    int decode_y_scale = 1; // This is not a scallop, it's a scaling.
    Image img = decode_image(rects, rect_count, palette, width, height,decode_x_scale,decode_y_scale);
    clock_t t1 = clock();

    save_bmp(bmp_filename, img);
    clock_t t2 = clock();

    printf("image size: %d x %d\n", width*decode_x_scale, height*decode_y_scale);
    printf("展開時間: %.4f 秒\n", (double)(t1 - t0) / CLOCKS_PER_SEC);
    printf("BMP保存時間: %.4f 秒\n", (double)(t2 - t1) / CLOCKS_PER_SEC);
    printf("復元完了: %s\n", bmp_filename);

    free(raw_data);
    free(palette);
    free(rects);
    free(img.pixels);
}

// =========================================================
// テストメイン
// =========================================================
int main() {
    const char *bmp_input = "googlehome.bmp";
    const char *ivr_output = "test.ivr";
    const char *bmp_restored = "test512.bmp";

    bmp_to_ivr(bmp_input, ivr_output);
    ivr_to_bmp(ivr_output, bmp_restored);

    return 0;
}