import { invoke } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";
import { getCurrentWindow } from "@tauri-apps/api/window";

// 要素の取得
const btnOpen = document.querySelector("#btn-open") as HTMLButtonElement;
const btnConvert = document.querySelector("#btn-convert") as HTMLButtonElement;
const btnSaveIvr = document.querySelector("#btn-save-ivr") as HTMLButtonElement;
const btnSaveBmp = document.querySelector("#btn-save-bmp") as HTMLButtonElement;


const sliderX = document.querySelector("#slider-x") as HTMLInputElement;
const sliderY = document.querySelector("#slider-y") as HTMLInputElement;
const sliderZstd = document.querySelector("#slider-zstd") as HTMLInputElement;
const previewImg = document.querySelector("#preview-img") as HTMLImageElement;
const logBox = document.querySelector("#log-box") as HTMLElement;
const appElement = document.getElementById("app")!;

let currentIvrData: Uint8Array | null = null;
let currentBmpData: Uint8Array | null = null;
let selectedPath = "";

function log(msg: string) {
  const p = document.createElement("p");
  p.style.margin = "2px 0";
  p.textContent = `> ${msg}`;
  logBox.appendChild(p);
  logBox.scrollTo(0, logBox.scrollHeight);
}

async function runConversion() {
  if (!selectedPath) {
    log("Error: No file selected.");
    return;
  }

  try {
    log("Converting...");
    // Rust側を呼び出し
    const res = await invoke<{ bmp_data: number[], ivr_data: number[], stats: string }>(
      "convert_and_preview", 
      {
        inputPath: selectedPath,
        sx: parseInt(sliderX.value),
        sy: parseInt(sliderY.value),
        zstdLevel: parseInt(sliderZstd.value) 
      }
    );

    currentIvrData = new Uint8Array(res.ivr_data);
    currentBmpData = new Uint8Array(res.bmp_data);

    // プレビュー表示
    const blob = new Blob([currentBmpData.buffer as any], { type: "image/bmp" });
    if (previewImg.src) URL.revokeObjectURL(previewImg.src); 
    previewImg.src = URL.createObjectURL(blob);

    // 画像が表示されるタイミングで要素を切り替える
    const placeholder = document.getElementById("drop-placeholder")!;
    placeholder.style.display = "none";      // 案内を消す
    previewImg.style.display = "block";    // 画像を出す

    // 統計を表示
    logBox.innerHTML = `<pre style="white-space: pre-wrap; color: #00ff99; background: #000; padding: 10px; font-family: monospace;">${res.stats}</pre>`;

    // ボタン有効化
    btnSaveIvr.disabled = false;
    btnSaveBmp.disabled = false;
    log("Success: Preview updated.");

  } catch (err) {
    console.error(err);
    logBox.innerHTML = `<span style="color: #ff6666;">Error: ${err}</span>`;
  }
}

// 1. ファイル選択ボタン
btnOpen.onclick = async () => {
  const selected = await open({
    filters: [
      { name: 'Supported Images', extensions: ['bmp', 'png', 'ivr', 'jpg', 'jpeg'] }
    ]
  });
  if (selected && !Array.isArray(selected)) {
    selectedPath = selected;
    log(`Selected: ${selected.split(/[\\\/]/).pop()}`);
  }
};

// 2. 変換ボタン（関数を呼ぶだけにする）
btnConvert.onclick = runConversion;

// 3. IVR保存
btnSaveIvr.onclick = async () => {
  if (!currentIvrData) return;
  const path = await save({ 
    defaultPath: 'output.ivr',
    filters: [{ name: 'IVR', extensions: ['ivr'] }] 
  });
  
  if (path) {
    try {
      await invoke("save_file", { path, data: Array.from(currentIvrData) });
      log(`Saved IVR: ${path.split(/[\\\/]/).pop()}`);
    } catch (e) {
      log(`Save Error: ${e}`);
    }
  }
};

// 4. BMP保存
btnSaveBmp.onclick = async () => {
  if (!currentBmpData) return;
  const path = await save({ 
    defaultPath: 'preview.bmp',
    filters: [{ name: 'BMP', extensions: ['bmp'] }] 
  });
  
  if (path) {
    try {
      await invoke("save_file", { path, data: Array.from(currentBmpData) });
      log(`Saved BMP: ${path.split(/[\\\/]/).pop()}`);
    } catch (e) {
      log(`Save Error: ${e}`);
    }
  }
};

// ドラッグ&ドロップのイベント
getCurrentWindow().onDragDropEvent((event) => {
  // payload.type を一旦 string として扱うことで、姑の「型が違う」という怒りをスルーします
  const eventType = event.payload.type as string;

  if (eventType === 'hover' || eventType === 'enter' || eventType === 'over') {
    // 進入時または重なっている時
    appElement.classList.add("drag-over");
  } 
  else if (event.payload.type === 'drop') {
    // ドロップされた時
    appElement.classList.remove("drag-over");
    
    const filePath = event.payload.paths[0]; 
    if (filePath) {
      selectedPath = filePath;
      log(`Dropped: ${filePath.split(/[\\\/]/).pop()}`);
      runConversion(); 
    }
  } 
  else {
    // 離れた時(leave)やキャンセル時
    appElement.classList.remove("drag-over");
  }
});

// スライダー連動
sliderX.oninput = () => document.getElementById("val-x")!.textContent = sliderX.value;
sliderY.oninput = () => document.getElementById("val-y")!.textContent = sliderY.value;
sliderZstd.oninput = () => document.getElementById("val-zstd")!.textContent = sliderZstd.value;