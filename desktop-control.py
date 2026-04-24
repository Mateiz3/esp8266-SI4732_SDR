import tkinter as tk
from tkinter import ttk
import serial
import threading
import json
import time
import math

# ==========================================
# Configuration
# ==========================================
SERIAL_PORT = '/dev/ttyUSB0'  # <-- CHANGE THIS
BAUD_RATE = 115200

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    time.sleep(2) 
except Exception as e:
    print(f"❌ ERROR: Could not open {SERIAL_PORT}.")
    exit()

def send_command(cmd):
    if ser.is_open:
        ser.write((cmd + '\n').encode('utf-8'))

def listen_to_radio():
    while True:
        if ser.is_open and ser.in_waiting > 0:
            try:
                raw_line = ser.readline().decode('utf-8').strip()
                if raw_line.startswith('{') and raw_line.endswith('}'):
                    update_gui(json.loads(raw_line))
            except Exception as e:
                pass

is_dragging_vol = False
is_dragging_bfo = False

def tune_up(): send_command("UP")
def tune_down(): send_command("DOWN")
def tune_direct(event=None):
    freq = entry_freq.get()
    if freq:
        send_command(f"TUNE={freq}")
        entry_freq.delete(0, tk.END)

def change_band(event=None): send_command(f"BAND={band_var.get()}")
def change_mod(event=None): send_command(f"MOD={mod_var.get()}")

def on_vol_press(event): global is_dragging_vol; is_dragging_vol = True
def on_vol_release(event):
    global is_dragging_vol; is_dragging_vol = False
    send_command(f"VOL={int(float(vol_slider.get()))}")

def on_bfo_press(event): global is_dragging_bfo; is_dragging_bfo = True
def on_bfo_release(event):
    global is_dragging_bfo; is_dragging_bfo = False
    send_command(f"BFO={int(float(bfo_slider.get()))}")

def reset_bfo(): send_command("BFO=0")

# --- Custom Radial Gauge Class ---
class RadialGauge(tk.Canvas):
    def __init__(self, parent, title, max_val, unit="", **kwargs):
        super().__init__(parent, width=150, height=90, bg="#121212", highlightthickness=0, **kwargs)
        self.title = title
        self.max_val = max_val
        self.unit = unit
        self.cx, self.cy = 75, 70
        self.r = 45
        self.w = 12
        self.draw_background()
        self.fill_arc = None
        self.val_text = None
        self.set_value(0)
        
    def draw_background(self):
        x0, y0 = self.cx - self.r, self.cy - self.r
        x1, y1 = self.cx + self.r, self.cy + self.r
        self.create_arc(x0, y0, x1, y1, start=0, extent=180, style="arc", outline="#333", width=self.w)
        self.create_text(self.cx, self.cy + 15, text=self.title, fill="#888", font=("Arial", 9, "bold"))
        
    def set_value(self, val):
        val = max(0, min(val, self.max_val))
        extent_deg = (val / self.max_val) * 180
        x0, y0 = self.cx - self.r, self.cy - self.r
        x1, y1 = self.cx + self.r, self.cy + self.r
        
        if self.fill_arc: self.delete(self.fill_arc)
        if self.val_text: self.delete(self.val_text)
            
        self.fill_arc = self.create_arc(x0, y0, x1, y1, start=180, extent=-extent_deg, style="arc", outline="#00ffcc", width=self.w)
        self.val_text = self.create_text(self.cx, self.cy - 10, text=f"{val} {self.unit}".strip(), fill="#fff", font=("Arial", 11, "bold"))

def update_gui(data):
    def _update():
        # --- UNIT FORMATTING LOGIC ---
        freq = data.get('freq', 0)
        unit = data.get('unit', 'MHz')
        if unit == 'MHz':
            lbl_freq.config(text=f"{freq:.3f} MHz")
        else:
            lbl_freq.config(text=f"{int(freq)} kHz")
        
        current_band = data.get('mode', '--')
        current_mod = data.get('mod', '')
        lbl_mode.config(text="FM" if current_band == "FM" else f"{current_band} [{current_mod}]")
        
        # RDS & Stereo Updates
        lbl_rds_name.config(text=data.get('rds_name', ''))
        lbl_rds_text.config(text=data.get('rds_text', ''))
        lbl_rds_time.config(text=data.get('rds_time', ''))
        
        if current_band == "FM":
            is_stereo = data.get('stereo', False)
            lbl_stereo.config(text=" STEREO " if is_stereo else " MONO ", bg="#008CBA" if is_stereo else "#555", fg="white")
        else:
            lbl_stereo.config(text="", bg="#222", fg="#222")

        pty = data.get('rds_pty', '')
        lbl_rds_pty.config(text=f" {pty} ", bg="#e91e63", fg="white") if pty and pty != "None" else lbl_rds_pty.config(text="", bg="#222")
        
        # Dropdown States
        if band_var.get() != current_band: band_var.set(current_band)
        if current_band == "FM":
            mod_var.set("FM")
            mod_dropdown.config(state="disabled")
            frame_bfo.pack_forget()
        else:
            mod_dropdown.config(state="readonly")
            if mod_var.get() != current_mod: mod_var.set(current_mod)
            
            if current_mod in ["LSB", "USB"]:
                frame_bfo.pack(pady=5, before=frame_telemetry)
                if not is_dragging_bfo:
                    bfo_slider.set(data.get('bfo', 0))
                    lbl_bfo_val.config(text=f"{data.get('bfo', 0)} Hz")
            else:
                frame_bfo.pack_forget()

        # Update Gauges
        gauge_rssi.set_value(int(data.get('rssi', 0)))
        gauge_snr.set_value(int(data.get('snr', 0)))
        
        if not is_dragging_vol: vol_slider.set(data.get('vol', 30))
            
    root.after(0, _update)

# ==========================================
# Build the User Interface 
# ==========================================
root = tk.Tk()
root.title("SDR Control")
root.geometry("380x600") 
root.configure(bg="#121212")

frame_display = tk.Frame(root, bg="#222", bd=2, relief="groove")
frame_display.pack(pady=15, padx=20, fill="x")

lbl_freq = tk.Label(frame_display, text="--.-- MHz", font=("Courier", 32, "bold"), bg="#222", fg="#00ffcc")
lbl_freq.pack(pady=(10, 0))

frame_mode = tk.Frame(frame_display, bg="#222")
frame_mode.pack(pady=(0, 2))
lbl_mode = tk.Label(frame_mode, text="--", font=("Arial", 12), bg="#222", fg="#888")
lbl_mode.pack(side="left", padx=5)

lbl_stereo = tk.Label(frame_mode, text="", font=("Arial", 9, "bold"), bg="#222", fg="#222", borderwidth=2, relief="flat")
lbl_stereo.pack(side="left", padx=(0, 5))

lbl_rds_pty = tk.Label(frame_mode, text="", font=("Arial", 9, "bold"), bg="#222", fg="#222", borderwidth=2, relief="flat")
lbl_rds_pty.pack(side="left")

lbl_rds_time = tk.Label(frame_display, text="", font=("Arial", 9), bg="#222", fg="#888")
lbl_rds_time.pack(pady=(0, 5))
lbl_rds_name = tk.Label(frame_display, text="", font=("Arial", 12, "bold"), bg="#222", fg="#ffeb3b")
lbl_rds_name.pack()
lbl_rds_text = tk.Label(frame_display, text="", font=("Arial", 10), bg="#222", fg="#bbb", wraplength=280)
lbl_rds_text.pack(pady=(0, 10))

frame_tune = tk.Frame(root, bg="#121212")
frame_tune.pack(pady=5)
band_var = tk.StringVar(value="FM")
band_dropdown = ttk.Combobox(frame_tune, textvariable=band_var, values=("FM", "MW", "SW"), width=4, state="readonly")
band_dropdown.pack(side="left", padx=2)
band_dropdown.bind("<<ComboboxSelected>>", change_band)
mod_var = tk.StringVar(value="FM")
mod_dropdown = ttk.Combobox(frame_tune, textvariable=mod_var, values=("AM", "LSB", "USB"), width=4, state="disabled")
mod_dropdown.pack(side="left", padx=2)
mod_dropdown.bind("<<ComboboxSelected>>", change_mod)

entry_freq = tk.Entry(frame_tune, font=("Arial", 14), width=8, justify="center")
entry_freq.pack(side="left", padx=5)
entry_freq.bind("<Return>", tune_direct) 
tk.Button(frame_tune, text="GO", bg="#008CBA", fg="white", command=tune_direct).pack(side="left")

frame_btns = tk.Frame(root, bg="#121212")
frame_btns.pack(pady=10)
tk.Button(frame_btns, text="<< DOWN", bg="#444", fg="white", width=10, command=tune_down).pack(side="left", padx=10)
tk.Button(frame_btns, text="UP >>", bg="#444", fg="white", width=10, command=tune_up).pack(side="left", padx=10)

tk.Label(root, text="Volume", bg="#121212", fg="white").pack()
vol_slider = ttk.Scale(root, from_=0, to=63, orient="horizontal", length=200)
vol_slider.pack(pady=5)
vol_slider.bind("<ButtonPress-1>", on_vol_press)
vol_slider.bind("<ButtonRelease-1>", on_vol_release)

frame_bfo = tk.Frame(root, bg="#121212")
frame_bfo_header = tk.Frame(frame_bfo, bg="#121212")
frame_bfo_header.pack(fill="x")
tk.Label(frame_bfo_header, text="SSB Fine Tune (BFO)", bg="#121212", fg="#ff9800", font=("Arial", 9, "bold")).pack(side="left")
lbl_bfo_val = tk.Label(frame_bfo_header, text="0 Hz", bg="#121212", fg="#bbb", font=("Courier", 9))
lbl_bfo_val.pack(side="right")
bfo_slider = ttk.Scale(frame_bfo, from_=-3000, to=3000, orient="horizontal", length=200)
bfo_slider.pack(pady=2)
bfo_slider.bind("<ButtonPress-1>", on_bfo_press)
bfo_slider.bind("<ButtonRelease-1>", on_bfo_release)
tk.Button(frame_bfo, text="ZERO BEAT", bg="#333", fg="#ff9800", font=("Arial", 8), command=reset_bfo).pack()

# --- The New Gauge Telemetry Section ---
frame_telemetry = tk.Frame(root, bg="#121212")
frame_telemetry.pack(pady=15, side="bottom")

gauge_rssi = RadialGauge(frame_telemetry, "RSSI", max_val=80, unit="dBuV")
gauge_rssi.pack(side="left", padx=10)

gauge_snr = RadialGauge(frame_telemetry, "SNR", max_val=40, unit="dB")
gauge_snr.pack(side="left", padx=10)

# ==========================================
thread = threading.Thread(target=listen_to_radio, daemon=True)
thread.start()
send_command("STATUS")
root.mainloop()
ser.close()