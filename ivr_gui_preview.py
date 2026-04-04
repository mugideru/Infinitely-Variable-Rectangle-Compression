import customtkinter as ctk
from PIL import Image, ImageTk
import subprocess
import os
from tkinter import filedialog
import io

class IVRPreviewer(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("IVR Image Compressor - Previewer")
        self.geometry("1100x900")

        self.input_path = ""
        self.ivr_path = "test.ivr"
        self.preview_path = "preview_temp.bmp" # C側が書き出すファイル名

        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # --- Sidebar ---
        self.sidebar = ctk.CTkFrame(self, width=250)
        self.sidebar.grid(row=0, column=0, padx=10, pady=10, sticky="nsew")

        ctk.CTkButton(self.sidebar, text="1. Open BMP", command=self.select_file).pack(pady=20, padx=10)

        self.lbl_scale_x = ctk.CTkLabel(self.sidebar, text="Scale X: 1")
        self.lbl_scale_x.pack()
        self.slider_x = ctk.CTkSlider(self.sidebar, from_=1, to=8, number_of_steps=7, command=self.update_labels)
        self.slider_x.set(1)
        self.slider_x.pack(pady=5, padx=10)

        self.lbl_scale_y = ctk.CTkLabel(self.sidebar, text="Scale Y: 1")
        self.lbl_scale_y.pack()
        self.slider_y = ctk.CTkSlider(self.sidebar, from_=1, to=8, number_of_steps=7, command=self.update_labels)
        self.slider_y.set(1)
        self.slider_y.pack(pady=5, padx=10)

        ctk.CTkButton(self.sidebar, text="2. Convert & Preview", fg_color="green", 
                      command=self.run_conversion).pack(pady=30, padx=10)

        # --- Main View (Scrollable) ---
        self.main_view = ctk.CTkScrollableFrame(self)
        self.main_view.grid(row=0, column=1, padx=10, pady=10, sticky="nsew")
        
        self.preview_label = ctk.CTkLabel(self.main_view, text="")
        self.preview_label.pack()

        # --- Log ---
        self.log_box = ctk.CTkTextbox(self, height=200, font=("Consolas", 11))
        self.log_box.grid(row=1, column=0, columnspan=2, padx=10, pady=10, sticky="nsew")

    def update_labels(self, _=None):
        self.lbl_scale_x.configure(text=f"Scale X: {int(self.slider_x.get())}")
        self.lbl_scale_y.configure(text=f"Scale Y: {int(self.slider_y.get())}")

    def select_file(self):
        path = filedialog.askopenfilename(filetypes=[("BMP files", "*.bmp")])
        if path:
            self.input_path = path
            self.log(f"Selected: {path}")

    def log(self, message):
        self.log_box.insert("end", message + "\n")
        self.log_box.see("end")

    def run_conversion(self):
        if not self.input_path: return
        sx, sy = int(self.slider_x.get()), int(self.slider_y.get())
        
        exe = r"C:\create_file\ivr_project\ivr_converter.exe"
        cmd = [exe, self.input_path, self.ivr_path, str(sx), str(sy)]

        try:
            # 実行
            result = subprocess.run(cmd, capture_output=True, check=True)
            
            if result.stderr:
                log_text = result.stderr.decode("utf-8", errors="replace")
                self.log(log_text) # ログボックスに出力

            # 画像データ (stdout) の処理
            img_bytes = result.stdout
            if img_bytes:
                img = Image.open(io.BytesIO(img_bytes))
                
                # 表示サイズ調整
                max_w = 1200
                w, h = img.width, img.height
                if w > max_w or h > max_w:
                    ratio = min(max_w/w, max_w/h)
                    w, h = int(w * ratio), int(h * ratio)

                ctk_img = ctk.CTkImage(light_image=img, dark_image=img, size=(w, h))
                self.preview_label.configure(image=ctk_img, text="")
                self.log(f"Success: Received {len(img_bytes)} bytes from RAM.")
            
        except subprocess.CalledProcessError as e:
            # エラー時も stderr に詳細が入っているはず
            err_msg = e.stderr.decode("utf-8", errors="replace")
            self.log(f"C Error:\n{err_msg}")
        except Exception as e:
            self.log(f"System Error: {str(e)}")

if __name__ == "__main__":
    app = IVRPreviewer()
    app.mainloop()