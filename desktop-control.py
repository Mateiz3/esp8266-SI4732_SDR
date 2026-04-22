import tkinter as tk
from tkinter import ttk
import serial
import threading
import json
import time

# ==========================================
# Configuration
# ==========================================
SERIAL_PORT = '/dev/ttyUSB0'  # <-- CHANGE THIS TO YOUR PORT
BAUD_RATE = 115200

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    time.sleep(2) 
except Exception as e:
    print(f"❌ ERROR: Could not open {SERIAL_PORT}. Is it plugged in?")
    print(f"Details: {e}")
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
                    data = json.loads(raw_line)
                    update_gui(data)
                else:
                    print(f"ESP8266: {raw_line}")
            except Exception as e:
                pass

is_dragging_vol = False

def tune_up(): send_command("UP")
def tune_down(): send_command("DOWN")

def tune_direct(event=None):
    freq = entry_freq.get()
    if freq:
        send_command(f"TUNE={freq}")
        entry_freq.delete(0, tk.END)

def change_band(event=None):
    band = band_var.get()
    send_command(f"BAND={band}")

def on_vol_press(event):
    global is_dragging_vol
    is_dragging_vol = True

def on_vol_release(event):
    global is_dragging_vol
    is_dragging_vol = False
    val = vol_slider.get()
    send_command(f"VOL={int(float(val))}")

def update_gui(data):
    def _update():
        rssi = int(data.get('rssi', 0))
        snr = int(data.get('snr', 0))
        lbl_freq.config(text=f"{data.get('freq', 0):.3f}")
        lbl_mode.config(text=data.get('mode', '--'))
        
        # Update RDS fields
        lbl_rds_name.config(text=data.get('rds_name', ''))
        lbl_rds_text.config(text=data.get('rds_text', ''))
        
        # Display Genre (PTY) if available
        pty = data.get('rds_pty', '')
        if pty and pty != "None":
            lbl_rds_pty.config(text=f" {pty} ", bg="#e91e63", fg="white")
        else:
            lbl_rds_pty.config(text="", bg="#222") # Hide if empty
        
        # Update Dropdown
        if band_var.get() != data.get('mode', 'FM'):
            band_var.set(data.get('mode', 'FM'))

        lbl_rssi.config(text=f"RSSI: {rssi} dBuV")
        lbl_snr.config(text=f"SNR: {snr} dB")
        rssi_bar["value"] = max(0, min(rssi, 80))
        snr_bar["value"] = max(0, min(snr, 40))
        
        if not is_dragging_vol:
            vol_slider.set(data.get('vol', 30))
            
    root.after(0, _update)

# ==========================================
# Build the User Interface (Dark Mode)
# ==========================================
root = tk.Tk()
root.title("SDR Control")
root.geometry("350x520") 
root.configure(bg="#121212")

# --- Display Frame ---
frame_display = tk.Frame(root, bg="#222", bd=2, relief="groove")
frame_display.pack(pady=15, padx=20, fill="x")

lbl_freq = tk.Label(frame_display, text="--.--", font=("Courier", 36, "bold"), bg="#222", fg="#00ffcc")
lbl_freq.pack(pady=(10, 0))

# Mode and PTY Container
frame_mode = tk.Frame(frame_display, bg="#222")
frame_mode.pack(pady=(0, 5))

lbl_mode = tk.Label(frame_mode, text="--", font=("Arial", 12), bg="#222", fg="#888")
lbl_mode.pack(side="left", padx=5)

lbl_rds_pty = tk.Label(frame_mode, text="", font=("Arial", 9, "bold"), bg="#222", fg="#222", borderwidth=2, relief="flat")
lbl_rds_pty.pack(side="left")

lbl_rds_name = tk.Label(frame_display, text="", font=("Arial", 12, "bold"), bg="#222", fg="#ffeb3b")
lbl_rds_name.pack()

lbl_rds_text = tk.Label(frame_display, text="", font=("Arial", 10), bg="#222", fg="#bbb", wraplength=280)
lbl_rds_text.pack(pady=(0, 10))

# --- Direct Tuning & Band Dropdown ---
frame_tune = tk.Frame(root, bg="#121212")
frame_tune.pack(pady=5)

band_var = tk.StringVar(value="FM")
band_dropdown = ttk.Combobox(frame_tune, textvariable=band_var, values=("FM", "MW", "SW"), width=4, state="readonly")
band_dropdown.pack(side="left", padx=5)
band_dropdown.bind("<<ComboboxSelected>>", change_band)

entry_freq = tk.Entry(frame_tune, font=("Arial", 14), width=8, justify="center")
entry_freq.pack(side="left", padx=5)
entry_freq.bind("<Return>", tune_direct) 
btn_go = tk.Button(frame_tune, text="GO", bg="#008CBA", fg="white", command=tune_direct)
btn_go.pack(side="left")

# --- Up / Down Buttons ---
frame_btns = tk.Frame(root, bg="#121212")
frame_btns.pack(pady=10)
btn_down = tk.Button(frame_btns, text="<< DOWN", bg="#444", fg="white", width=10, command=tune_down)
btn_down.pack(side="left", padx=10)
btn_up = tk.Button(frame_btns, text="UP >>", bg="#444", fg="white", width=10, command=tune_up)
btn_up.pack(side="left", padx=10)

# --- Volume Slider ---
tk.Label(root, text="Volume", bg="#121212", fg="white").pack()
vol_slider = ttk.Scale(root, from_=0, to=63, orient="horizontal", length=200)
vol_slider.pack(pady=5)
vol_slider.bind("<ButtonPress-1>", on_vol_press)
vol_slider.bind("<ButtonRelease-1>", on_vol_release)

# --- Telemetry ---
frame_telemetry = tk.Frame(root, bg="#121212")
frame_telemetry.pack(pady=15, side="bottom")
lbl_rssi = tk.Label(frame_telemetry, text="RSSI: 0 dBuV", font=("Arial", 10), bg="#121212", fg="#bbb", width=15)
lbl_rssi.pack(side="left")
lbl_snr = tk.Label(frame_telemetry, text="SNR: 0 dB", font=("Arial", 10), bg="#121212", fg="#bbb", width=15)
lbl_snr.pack(side="left")

# --- Signal Visuals ---
frame_signal = tk.Frame(root, bg="#121212")
frame_signal.pack(padx=20, pady=5, fill="x")

tk.Label(frame_signal, text="RSSI", bg="#121212", fg="#bbb", anchor="w").pack(fill="x")
rssi_bar = ttk.Progressbar(frame_signal, orient="horizontal", mode="determinate", maximum=80)
rssi_bar.pack(fill="x", pady=(0, 8))

tk.Label(frame_signal, text="SNR", bg="#121212", fg="#bbb", anchor="w").pack(fill="x")
snr_bar = ttk.Progressbar(frame_signal, orient="horizontal", mode="determinate", maximum=40)
snr_bar.pack(fill="x")

# ==========================================
# Startup
# ==========================================
thread = threading.Thread(target=listen_to_radio, daemon=True)
thread.start()

send_command("STATUS")

root.mainloop()

ser.close()