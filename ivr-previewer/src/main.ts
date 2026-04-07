import { invoke } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";

// 要素の取得と型確定
const btnOpen = document.querySelector("#btn-open") as HTMLButtonElement;
const btnConvert = document.querySelector("#btn-convert") as HTMLButtonElement;
const btnSaveIvr = document.querySelector("#btn-save-ivr") as HTMLButtonElement;
const btnSaveBmp = document.querySelector("#btn-save-bmp") as HTMLButtonElement;

const sliderX = document.querySelector("#slider-x") as HTMLInputElement;
const sliderY = document.querySelector("#slider-y") as HTMLInputElement;
const previewImg = document.querySelector("#preview-img") as HTMLImageElement;
const logBox = document.querySelector("#log-box") as HTMLElement;

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

// 1. ファイル選択
btnOpen.onclick = async () => {
  const selected = await open({
    filters: [
      { name: 'Supported Images', extensions: ['bmp', 'png', 'ivr'] } // ★ivrとpngを追加
    ]
  });
  if (selected && !Array.isArray(selected)) {
    selectedPath = selected;
    log(`Selected: ${selected.split(/[\\\/]/).pop()}`);
  }
};

// 2. 変換・プレビュー実行
btnConvert.onclick = async () => {
  if (!selectedPath) {
    log("Error: No file selected.");
    return;
  }

  try {
    log("Converting...");
    const res = await invoke<{ bmp_data: number[], ivr_data: number[], stats: string }>(
      "convert_and_preview", 
      {
        inputPath: selectedPath,
        sx: parseInt(sliderX.value),
        sy: parseInt(sliderY.value)
      }
    );

    currentIvrData = new Uint8Array(res.ivr_data);
    currentBmpData = new Uint8Array(res.bmp_data);

    // 画像を表示 (姑を黙らせる as any)
    const blob = new Blob([currentBmpData.buffer as any], { type: "image/bmp" });
    if (previewImg.src) URL.revokeObjectURL(previewImg.src); // メモリ解放
    previewImg.src = URL.createObjectURL(blob);

    // 統計を表示
    logBox.innerHTML = `<pre style="white-space: pre-wrap; color: #00ff99; background: #000; padding: 10px;">${res.stats}</pre>`;

    // ボタン有効化
    btnSaveIvr.disabled = false;
    btnSaveBmp.disabled = false;
    log("Success: Preview updated.");

  } catch (err) {
    console.error(err);
    logBox.innerHTML = `<span style="color: #ff6666;">Error: ${err}</span>`;
  }
};

// 3. IVR保存
btnSaveIvr.onclick = async () => {
  if (!currentIvrData) return;
  const path = await save({ 
    defaultPath: 'output.ivr',
    filters: [{ name: 'IVR', extensions: ['ivr'] }] 
  });
  
  if (path) {
    try {
      // dataは number[] で送る必要がある
      await invoke("save_file", { path, data: Array.from(currentIvrData) });
      log(`Saved: ${path.split(/[\\\/]/).pop()}`);
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
      log(`Saved: ${path.split(/[\\\/]/).pop()}`);
    } catch (e) {
      log(`Save Error: ${e}`);
    }
  }
};

// スライダー連動
sliderX.oninput = () => document.getElementById("val-x")!.textContent = sliderX.value;
sliderY.oninput = () => document.getElementById("val-y")!.textContent = sliderY.value;