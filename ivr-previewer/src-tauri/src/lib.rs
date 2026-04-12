use std::fs::File;
use std::io::{self, Read, Cursor};
use std::time::Instant;
use image::{GenericImageView, ImageEncoder, ColorType, load_from_memory};
use image::codecs::png::PngEncoder;

const ZSTD_COMPRESSION_LEVEL: i32 = 19;
const IVR_MAX_PALETTE_SIZE: u32 = 16_777_216;
const IVR_MAX_RECT_COUNT: u32 = 16_000_000;
const MAX_IMAGE_PIXELS: u32 = 268435456;

// --- データ構造定義 ---

#[derive(Clone, Copy, Default)]
struct Color {
    r: u8,
    g: u8,
    b: u8,
    a: u8, 
}

#[derive(serde::Serialize)]
struct ConversionResult {
    png_data: Vec<u8>,
    ivr_data: Vec<u8>,
    stats: String,
}

struct Rect {
    w: u32,
    h: u32,
    c_idx: u32,
}

struct Image {
    width: u32,
    height: u32,
    pixels: Vec<Color>,
}

struct BitWriter {
    data: Vec<u8>,
    bit_buffer: u64,
    bits_in_buffer: i32,
}

struct BitReader<'a> {
    data: &'a [u8],
    byte_pos: usize,
    bit_pos: i32,
    eof: bool,
}

// --- 補助関数 ---

fn bits_needed(n: u32) -> i32 {
    if n <= 1 { 1 } else { (32 - (n - 1).leading_zeros()) as i32 }
}

// --- ビット操作関連 ---

impl BitWriter {
    fn new() -> Self {
        Self {
            data: Vec::with_capacity(1024 * 1024),
            bit_buffer: 0,
            bits_in_buffer: 0,
        }
    }

    fn write_bits(&mut self, value: u32, width: i32) {
        if width <= 0 { return; }

        while self.bits_in_buffer + width > 56 {
            self.data.push((self.bit_buffer >> (self.bits_in_buffer - 8)) as u8);
            self.bits_in_buffer -= 8;
        }

        let mask = if width == 0 { 0 } else { (1u64 << width) - 1 };
        self.bit_buffer = (self.bit_buffer << width) | ((value as u64) & mask);
        self.bits_in_buffer += width;

        while self.bits_in_buffer >= 8 {
            self.data.push((self.bit_buffer >> (self.bits_in_buffer - 8)) as u8);
            self.bits_in_buffer -= 8;
        }
    }

    fn write_exp_golomb(&mut self, n: u32) {
        let mut bits = 0;
        let mut temp = n;
        while temp > 0 {
            bits += 1;
            temp >>= 1;
        }
        if bits > 1 {
            self.write_bits(0, bits - 1);
        }
        self.write_bits(n, bits);
    }

    fn finish(&mut self) {
        if self.bits_in_buffer > 0 {
            let pad = (8 - (self.bits_in_buffer % 8)) % 8;
            if pad > 0 {
                self.write_bits(0, pad);
            }
        }
    }
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self {
            data,
            byte_pos: 0,
            bit_pos: 0,
            eof: false,
        }
    }

    fn read_bits(&mut self, width: i32) -> u32 {
        let mut val = 0;
        for _ in 0..width {
            if self.byte_pos >= self.data.len() {
                self.eof = true;
                return 0;
            }
            val = (val << 1) | (((self.data[self.byte_pos] >> (7 - self.bit_pos)) & 1) as u32);
            self.bit_pos += 1;
            if self.bit_pos == 8 {
                self.bit_pos = 0;
                self.byte_pos += 1;
            }
        }
        val
    }

    fn read_exp_golomb(&mut self) -> u32 {
        let mut zeros = 0;
        while self.read_bits(1) == 0 && !self.eof {
            zeros += 1;
            if zeros > 32 {
                self.eof = true;
                return 0;
            }
        }
        if self.eof { return 0; }
        
        let mut val = 1;
        for _ in 0..zeros {
            val = (val << 1) | self.read_bits(1);
        }
        val
    }
}

// --- PNG/一般画像読み込み追加 ---
fn load_general_image(path: &str) -> Result<Image, String> {
    // ファイルを一旦メモリに読み込む
    let bytes = std::fs::read(path).map_err(|e| format!("File read error: {}", e))?;
    
    let dynamic_img = image::load_from_memory(&bytes)
        .map_err(|e| format!("Image load error: {}", e))?;
    
    let rgba_img = dynamic_img.to_rgba8();
    let (width, height) = rgba_img.dimensions();
    
    if width * height > MAX_IMAGE_PIXELS {
        return Err(format!("画像サイズが大きすぎます ({}x{})。", width, height));
    }

    let mut pixels = Vec::with_capacity((width * height) as usize);
    for p in rgba_img.pixels() {
        pixels.push(Color { r: p[0], g: p[1], b: p[2], a: p[3] });
    }
    
    Ok(Image { width, height, pixels })
}

// --- BMP読み書き関連 ---

// プレビュー用のBMP出力を 32-bit (アルファ対応) に変更
fn write_png_preview(img: &Image) -> Result<Vec<u8>, String> {
    if img.width == 0 || img.height == 0 || img.pixels.is_empty() {
        return Ok(Vec::new());
    }

    let mut png_data = Vec::new();
    let raw_pixels: Vec<u8> = img.pixels.iter()
        .flat_map(|p| [p.r, p.g, p.b, p.a])
        .collect();

    let encoder = PngEncoder::new(Cursor::new(&mut png_data));
    encoder.write_image(
        &raw_pixels,
        img.width,
        img.height,
        ColorType::Rgba8,
    ).map_err(|e| format!("PNG encoding error: {}", e))?;
    
    Ok(png_data)
}

fn load_bmp(filename: &str) -> io::Result<Image> {
    let mut f = File::open(filename)?;
    let mut h = [0u8; 54];
    f.read_exact(&mut h)?;

    if h[0] != b'B' || h[1] != b'M' {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Not a BMP"));
    }

    let dib_size = u32::from_le_bytes(h[14..18].try_into().unwrap());
    let bpp = u16::from_le_bytes(h[28..30].try_into().unwrap());
    let compression = u32::from_le_bytes(h[30..34].try_into().unwrap());
    let pixel_offset = u32::from_le_bytes(h[10..14].try_into().unwrap());

    if dib_size < 40 || bpp != 24 || compression != 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Unsupported BMP format"));
    }

    let width = u32::from_le_bytes(h[18..22].try_into().unwrap());
    let h_img = i32::from_le_bytes(h[22..26].try_into().unwrap());
    let height = h_img.unsigned_abs();

    if width == 0 || height == 0 || width * height > MAX_IMAGE_PIXELS {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid dimensions or too large"));
    }

    let mut skip = vec![0u8; (pixel_offset - 54) as usize];
    if pixel_offset < 54 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid BMP pixel offset"));
    }
    f.read_exact(&mut skip)?;

    let mut pixels = vec![Color::default(); (width * height) as usize];
    let row_size = (((width * 3) + 3) / 4) * 4;
    let mut row = vec![0u8; row_size as usize];

    for y in 0..height {
        f.read_exact(&mut row)?;
        let dy = if h_img > 0 { height - 1 - y } else { y };
        for x in 0..width {
            let idx = (x * 3) as usize;
            pixels[(dy * width + x) as usize] = Color {
                r: row[idx + 2],
                g: row[idx + 1],
                b: row[idx],
                a: 255, 
            };
        }
    }

    Ok(Image { width, height, pixels })
}

// --- エンコード処理関連 ---

fn make_palette(img: &Image) -> (Vec<Color>, Vec<u32>) {
    let total_pixels = img.pixels.len();
    let mut palette = Vec::with_capacity(65536);
    let mut indexed = vec![0u32; total_pixels];

    let mut current_hash_size = 65536u32;
    let mut table = vec![(0u32, 0u32, false); current_hash_size as usize]; 

    for i in 0..total_pixels {
        let p = &img.pixels[i];
        
        // RGBA全てを結合して一意な u32 を作る（ビットシフトより from_be_bytes が確実）
        let c = u32::from_be_bytes([p.r, p.g, p.b, p.a]);
        
        let mut h_val = c;
        h_val = ((h_val >> 16) ^ h_val).wrapping_mul(0x45d9f3b);
        h_val = ((h_val >> 16) ^ h_val).wrapping_mul(0x45d9f3b);
        h_val = (h_val >> 16) ^ h_val;

        let mut mask = current_hash_size - 1;
        let mut h = (h_val & mask) as usize;

        while table[h].2 {
            if table[h].0 == c { break; }
            h = (h + 1) & (mask as usize);
        }

        if !table[h].2 {
            if (palette.len() as u32) * 10 > current_hash_size * 7 {
                let old_table = table;
                current_hash_size *= 2;
                mask = current_hash_size - 1;
                table = vec![(0, 0, false); current_hash_size as usize];
                
                for entry in old_table {
                    if entry.2 {
                        let mut nh_val = entry.0;
                        nh_val = ((nh_val >> 16) ^ nh_val).wrapping_mul(0x45d9f3b);
                        nh_val = ((nh_val >> 16) ^ nh_val).wrapping_mul(0x45d9f3b);
                        nh_val = (nh_val >> 16) ^ nh_val;
                        
                        let mut nh = (nh_val & mask) as usize;
                        while table[nh].2 { nh = (nh + 1) & (mask as usize); }
                        table[nh] = entry;
                    }
                }
                h = (h_val & mask) as usize;
                while table[h].2 { h = (h + 1) & (mask as usize); }
            }

            let pal_idx = palette.len() as u32;
            table[h] = (c, pal_idx, true);
            palette.push(*p);
            indexed[i] = pal_idx;
        } else {
            indexed[i] = table[h].1;
        }
    }

    (palette, indexed)
}

fn encode_image(indexed: &[u32], w: u32, h: u32) -> Vec<Rect> {
    let mut used = vec![false; (w * h) as usize];
    let mut rects = Vec::new();
    let mut curr_y = 0;
    let mut curr_x = 0;

    while curr_y < h {
        let mut found = false;
        while curr_y < h {
            if !used[(curr_y * w + curr_x) as usize] {
                found = true;
                break;
            }
            curr_x += 1;
            if curr_x >= w {
                curr_x = 0;
                curr_y += 1;
            }
        }
        if !found { break; }

        let c = indexed[(curr_y * w + curr_x) as usize];
        
        let mut w1 = 0;
        for i in curr_x..w {
            let idx = (curr_y * w + i) as usize;
            if indexed[idx] == c && !used[idx] { w1 += 1; } else { break; }
        }
        let mut h1 = 0;
        for j in curr_y..h {
            let mut ok = true;
            for i in curr_x..(curr_x + w1) {
                let idx = (j * w + i) as usize;
                if indexed[idx] != c || used[idx] { ok = false; break; }
            }
            if ok { h1 += 1; } else { break; }
        }

        let mut h2 = 0;
        for j in curr_y..h {
            let idx = (j * w + curr_x) as usize;
            if indexed[idx] == c && !used[idx] { h2 += 1; } else { break; }
        }
        let mut w2 = 0;
        for i in curr_x..w {
            let mut ok = true;
            for j in curr_y..(curr_y + h2) {
                let idx = (j * w + i) as usize;
                if indexed[idx] != c || used[idx] { ok = false; break; }
            }
            if ok { w2 += 1; } else { break; }
        }

        let (final_w, final_h) = if w1 * h1 >= w2 * h2 {
            (w1, h1)
        } else {
            (w2, h2)
        };

        rects.push(Rect { w: final_w, h: final_h, c_idx: c });

        for j in curr_y..(curr_y + final_h) {
            for i in curr_x..(curr_x + final_w) {
                used[(j * w + i) as usize] = true;
            }
        }
    }

    rects
}

fn decode_image(rects: &[Rect], palette: &[Color], w: u32, h: u32, scale_x: u32, scale_y: u32) -> Option<Image> {
    if w == 0 || h == 0 || scale_x == 0 || scale_y == 0 { return None; }
    
    let out_w = w * scale_x;
    let out_h = h * scale_y;
    let mut pixels = vec![Color::default(); (out_w * out_h) as usize];
    let mut used = vec![false; (w * h) as usize];

    let mut curr_x = 0;
    let mut curr_y = 0;

    for rect in rects {
        while curr_y < h && used[(curr_y * w + curr_x) as usize] {
            curr_x += 1;
            if curr_x >= w {
                curr_x = 0;
                curr_y += 1;
            }
        }
        if curr_y >= h { break; }

        let rw = rect.w;
        let rh = rect.h;
        let ci = rect.c_idx as usize;

        if ci >= palette.len() || curr_x + rw > w || curr_y + rh > h {
            return None;
        }

        let color = palette[ci];
        for j in curr_y..(curr_y + rh) {
            for i in curr_x..(curr_x + rw) {
                used[(j * w + i) as usize] = true;
            }
        }

        let dx0 = curr_x * scale_x;
        let dy0 = curr_y * scale_y;
        let dx1 = (curr_x + rw) * scale_x;
        let dy1 = (curr_y + rh) * scale_y;

        for y in dy0..dy1 {
            for x in dx0..dx1 {
                pixels[(y * out_w + x) as usize] = color;
            }
        }
    }
    if used.iter().any(|&v| !v) {
        return None;
    }
    Some(Image { width: out_w, height: out_h, pixels })
}

// --- IVR ファイル I/O ---

fn ivr_to_image(ivr_in: &str, sx: u32, sy: u32) -> io::Result<Image> {
    eprintln!("=== IVR -> Decoding ===\n");
    let t0 = Instant::now();

    let mut f = File::open(ivr_in)?;
    let mut magic = [0u8; 4];
    f.read_exact(&mut magic)?;
    if &magic != b"IVR1" {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "エラー: 無効な形式"));
    }

    let mut buf4 = [0u8; 4];
    f.read_exact(&mut buf4)?; let w = u32::from_le_bytes(buf4);
    f.read_exact(&mut buf4)?; let h = u32::from_le_bytes(buf4);
    f.read_exact(&mut buf4)?; let raw_size = u32::from_le_bytes(buf4);

    let mut z_buf = Vec::new();
    f.read_to_end(&mut z_buf)?;

    let raw = zstd::stream::decode_all(z_buf.as_slice())?;
    if raw.len() != raw_size as usize {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Decompression size mismatch"));
    }

    let mut br = BitReader::new(&raw);
    
    // ★ アルファ情報のデコード (9-bit)
    let is_variable_alpha = br.read_bits(1) == 1;
    let global_alpha = br.read_bits(8) as u8;

    let pal_sz = br.read_exp_golomb();
    let rect_cnt = br.read_exp_golomb();

    if pal_sz > IVR_MAX_PALETTE_SIZE || rect_cnt > IVR_MAX_RECT_COUNT {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Size exceeds limits"));
    }

    let mut pal = Vec::with_capacity(pal_sz as usize);
    let mut cur = Color::default();
    for _ in 0..pal_sz {
        cur.r = cur.r.wrapping_add(br.read_bits(8) as u8);
        cur.g = cur.g.wrapping_add(br.read_bits(8) as u8);
        cur.b = cur.b.wrapping_add(br.read_bits(8) as u8);
        
        // 可変ならパレットから復元、一律ならグローバル値を割り当て
        if is_variable_alpha {
            cur.a = cur.a.wrapping_add(br.read_bits(8) as u8);
        } else {
            cur.a = global_alpha;
        }
        pal.push(cur);
    }

    let mut rects = Vec::with_capacity(rect_cnt as usize);
    let c_bits = bits_needed(pal_sz);

    for _ in 0..rect_cnt {
        rects.push(Rect { w: br.read_exp_golomb(), h: 0, c_idx: 0 });
    }
    for i in 0..rect_cnt as usize { rects[i].h = br.read_exp_golomb(); }
    for i in 0..rect_cnt as usize { rects[i].c_idx = br.read_bits(c_bits); }

    let img = decode_image(&rects, &pal, w, h, sx, sy)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Decode error"))?;

    eprintln!("デコード時間: {:.4} 秒", t0.elapsed().as_secs_f64());
    Ok(img)
}

#[tauri::command]
async fn save_file(path: String, data: Vec<u8>) -> Result<(), String> {
    std::fs::write(path, data).map_err(|e| e.to_string())
}

#[tauri::command]
async fn convert_and_preview(
    input_path: String, 
    sx: u32, 
    sy: u32,
    zstd_level: i32 
) -> Result<ConversionResult, String> {
    let metadata = std::fs::metadata(&input_path).map_err(|e| e.to_string())?;
    let input_file_size = metadata.len(); 
    let t_total = Instant::now();

    // 1. 画像読み込み
    let lower_path = input_path.to_lowercase();
    let img = if lower_path.ends_with(".ivr") {
        ivr_to_image(&input_path, 1, 1).map_err(|e| e.to_string())?
    } else {
        load_general_image(&input_path)?
    };
    
    let mut is_variable_alpha = false;
    let global_alpha = img.pixels.first().map(|p| p.a).unwrap_or(255);
    for p in &img.pixels {
        if p.a != global_alpha {
            is_variable_alpha = true;
            break;
        }
    }

    // 2. パレット計測
    let t_pal = Instant::now();
    let (palette, indexed) = make_palette(&img);
    let dur_pal = t_pal.elapsed().as_secs_f64();

    // 3. エンコード計測
    let t_enc = Instant::now();
    let rects = encode_image(&indexed, img.width, img.height);
    let dur_enc = t_enc.elapsed().as_secs_f64();

    // 4. IVR作成計測 (zstd圧縮含む)
    let t_zip = Instant::now();
    let mut bw = BitWriter::new();
    bw.write_bits(is_variable_alpha as u32, 1);
    bw.write_bits(global_alpha as u32, 8);
    bw.write_exp_golomb(palette.len() as u32);
    bw.write_exp_golomb(rects.len() as u32);

    let mut last_c = Color::default();
    for p in &palette {
        bw.write_bits(p.r.wrapping_sub(last_c.r) as u32, 8);
        bw.write_bits(p.g.wrapping_sub(last_c.g) as u32, 8);
        bw.write_bits(p.b.wrapping_sub(last_c.b) as u32, 8);
        if is_variable_alpha {
            bw.write_bits(p.a.wrapping_sub(last_c.a) as u32, 8);
        }
        last_c = *p;
    }
    
    let color_bits = bits_needed(palette.len() as u32);
    for r in &rects { bw.write_exp_golomb(r.w); }
    for r in &rects { bw.write_exp_golomb(r.h); }
    for r in &rects { bw.write_bits(r.c_idx, color_bits); }
    bw.finish();

    let raw_len = u32::try_from(bw.data.len()).map_err(|_| "Data too large".to_string())?;
    let compressed = zstd::stream::encode_all(bw.data.as_slice(), zstd_level).map_err(|e| e.to_string())?;
    let zstd_len = compressed.len();

    let mut ivr_data = Vec::new();
    ivr_data.extend_from_slice(b"IVR1");
    ivr_data.extend_from_slice(&img.width.to_le_bytes());
    ivr_data.extend_from_slice(&img.height.to_le_bytes());
    ivr_data.extend_from_slice(&(raw_len as u32).to_le_bytes());
    ivr_data.extend_from_slice(&compressed);
    let dur_zip = t_zip.elapsed().as_secs_f64();

    // 5. プレビュー用デコード
    let t_dec = Instant::now();
    let preview_img = decode_image(&rects, &palette, img.width, img.height, sx, sy)
        .ok_or("Decode error")?;
    let png_data = write_png_preview(&preview_img)?;
    let dur_dec = t_dec.elapsed().as_secs_f64();

    // 統計計算
    let dur_total = t_total.elapsed().as_secs_f64();
    let compression_ratio = (zstd_len as f64 / input_file_size as f64) * 100.0;
    let alpha_status = if is_variable_alpha { "Variable (in Palette)" } else { "Uniform (in Header)" };
    
    let out_w = img.width.checked_mul(sx).ok_or("Output width overflow")?;
    let out_h = img.height.checked_mul(sy).ok_or("Output height overflow")?;

    let stats = format!(
        "=== File Info ===\n\
         Path       : {}\n\
         Input Size : {} bytes\n\
         -------------------\n\
         === Any -> IVR ===\n\
         Resolution : {} x {} (Raw)\n\
         Scale      : x{} , y{}\n\
         Output Res : {} x {}\n\
         Alpha Mode : {}\n\
         -------------------\n\
         Palette    : {} colors\n\
         Rectangles : {}\n\
         Raw Data   : {} bytes\n\
         Zstd Data  : {} bytes\n\
         Ratio      : {:.2} % of original\n\
         Zstd Level : {}\n\
         -------------------\n\
         Make Pal   : {:.4} s\n\
         Encode     : {:.4} s\n\
         Zip        : {:.4} s\n\
         Decode(PNG): {:.4} s\n\
         TOTAL      : {:.4} s",
        input_path, input_file_size, img.width, img.height, sx, sy, 
        out_w, out_h, alpha_status, 
        palette.len(), rects.len(), raw_len, zstd_len, compression_ratio, zstd_level,
        dur_pal, dur_enc, dur_zip, dur_dec, dur_total
    );

    Ok(ConversionResult { png_data, ivr_data, stats })
}
// エントリポイント
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init()) 
        .invoke_handler(tauri::generate_handler![
            convert_and_preview,
            save_file
        ]) 
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}