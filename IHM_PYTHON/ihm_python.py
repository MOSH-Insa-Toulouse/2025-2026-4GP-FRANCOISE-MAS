"""
IHM Python equivalente a l'app Android MoshCapteur.

Lit des lignes "valeur unite" (ex: "1.23 MOhm", "500.00 kOhm", "250 Ohm")
depuis un port serie (HC-05 Bluetooth = port COM sortant sous Windows, ou
cable USB Arduino direct).

Dependances :
    pip install pyserial matplotlib
"""

import csv
import threading
import time
import tkinter as tk
from collections import deque
from tkinter import messagebox, filedialog, ttk

import serial
import serial.tools.list_ports
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure


BAUDRATE = 9600
MAX_POINTS = 300


class MoshIHM:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("MoshCapteur - IHM Python")
        self.root.geometry("800x600")

        self.ser: serial.Serial | None = None
        self.running = False
        self.reader_thread: threading.Thread | None = None

        self.samples: list[tuple[float, float]] = []   # (t_s, R_ohm)
        self.values = deque(maxlen=MAX_POINTS)
        self.r0_ohm: float | None = None
        self.t0: float | None = None

        self._build_ui()
        self._refresh_ports()

    # -------------------- UI --------------------
    def _build_ui(self):
        top = ttk.Frame(self.root, padding=8)
        top.pack(fill=tk.X)

        ttk.Label(top, text="Port :").pack(side=tk.LEFT)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=25)
        self.port_combo.pack(side=tk.LEFT, padx=4)
        ttk.Button(top, text="Rafraichir", command=self._refresh_ports).pack(side=tk.LEFT)

        self.btn_connect = ttk.Button(top, text="Connexion", command=self._toggle_connect)
        self.btn_connect.pack(side=tk.LEFT, padx=8)

        self.status_dot = tk.Canvas(top, width=20, height=20, highlightthickness=0)
        self.status_dot.pack(side=tk.LEFT)
        self._dot_id = self.status_dot.create_oval(2, 2, 18, 18, fill="#BB0000", outline="")
        self.status_label = ttk.Label(top, text="Deconnecte")
        self.status_label.pack(side=tk.LEFT, padx=4)

        self.value_label = ttk.Label(self.root, text="-- MOhm",
                                     font=("Segoe UI", 36, "bold"), anchor="center")
        self.value_label.pack(fill=tk.X, pady=(10, 0))

        self.delta_label = ttk.Label(self.root, text="DR/R0 = --",
                                     font=("Segoe UI", 14), anchor="center")
        self.delta_label.pack(fill=tk.X)

        btns = ttk.Frame(self.root, padding=8)
        btns.pack(fill=tk.X)
        ttk.Button(btns, text="Tarer R0", command=self._tare).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=2)
        ttk.Button(btns, text="Sauver CSV", command=self._save_csv).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=2)
        ttk.Button(btns, text="Reset", command=self._clear).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=2)

        self.fig = Figure(figsize=(7, 3.5), facecolor="#111111")
        self.ax = self.fig.add_subplot(111, facecolor="#111111")
        self.ax.tick_params(colors="#BBBBBB")
        for s in self.ax.spines.values():
            s.set_color("#666666")
        self.line_plot, = self.ax.plot([], [], color="cyan", linewidth=1.5)
        self.ax.set_xlabel("echantillon", color="#BBBBBB")
        self.ax.set_ylabel("R (Ohm)", color="#BBBBBB")
        self.fig.tight_layout()

        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(200, self._refresh_plot)

    # -------------------- Ports & connexion --------------------
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.running:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("Port", "Selectionne un port COM.")
            return
        try:
            self.ser = serial.Serial(port, BAUDRATE, timeout=1)
        except serial.SerialException as e:
            messagebox.showerror("Erreur connexion", str(e))
            return

        self.running = True
        self._set_status(True)
        self.btn_connect.config(text="Deconnexion")
        self.reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self.reader_thread.start()

    def _disconnect(self):
        self.running = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self._set_status(False)
        self.btn_connect.config(text="Connexion")

    def _set_status(self, connected: bool):
        color = "#00AA44" if connected else "#BB0000"
        self.status_dot.itemconfig(self._dot_id, fill=color)
        self.status_label.config(text="Connecte" if connected else "Deconnecte")

    # -------------------- Lecture serie --------------------
    def _read_loop(self):
        assert self.ser is not None
        while self.running:
            try:
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode(errors="ignore").strip()
                if line:
                    self._handle_line(line)
            except Exception:
                break
        self.root.after(0, lambda: self._set_status(False))

    def _handle_line(self, line: str):
        parts = line.split()
        if len(parts) < 2:
            return
        try:
            v = float(parts[0].replace(",", "."))
        except ValueError:
            return
        unit = parts[1]
        factor = {"MOhm": 1e6, "kOhm": 1e3, "Ohm": 1.0}.get(unit)
        if factor is None:
            return

        ohm = v * factor
        t = time.time()
        if self.t0 is None:
            self.t0 = t
        self.samples.append((t - self.t0, ohm))
        self.values.append(ohm)

        def update():
            self.value_label.config(text=f"{v:.2f} {unit}")
            if self.r0_ohm and self.r0_ohm > 0:
                d = (ohm - self.r0_ohm) / self.r0_ohm * 100.0
                self.delta_label.config(
                    text=f"DR/R0 = {d:+.2f} %  (R0 = {self.r0_ohm:.2f} Ohm)"
                )

        self.root.after(0, update)

    # -------------------- Boutons --------------------
    def _tare(self):
        if not self.samples:
            messagebox.showinfo("Tare", "Aucune valeur recue.")
            return
        self.r0_ohm = self.samples[-1][1]
        messagebox.showinfo("Tare", f"R0 = {self.r0_ohm:.2f} Ohm")

    def _save_csv(self):
        if not self.samples:
            messagebox.showinfo("CSV", "Rien a sauvegarder.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv")],
            initialfile=f"mosh_{int(time.time())}.csv",
        )
        if not path:
            return
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "R_ohm"])
            for t, r in self.samples:
                w.writerow([f"{t:.3f}", f"{r:.3f}"])
        messagebox.showinfo("CSV", f"Sauve : {path}")

    def _clear(self):
        self.samples.clear()
        self.values.clear()
        self.t0 = None
        self.delta_label.config(text="DR/R0 = --")
        self.line_plot.set_data([], [])
        self.canvas.draw_idle()

    # -------------------- Graphe --------------------
    def _refresh_plot(self):
        if self.values:
            xs = range(len(self.values))
            ys = list(self.values)
            self.line_plot.set_data(list(xs), ys)
            self.ax.set_xlim(0, max(len(ys), 10))
            vmin, vmax = min(ys), max(ys)
            span = max(vmax - vmin, 1e-6)
            self.ax.set_ylim(vmin - 0.05 * span, vmax + 0.05 * span)
            self.canvas.draw_idle()
        self.root.after(200, self._refresh_plot)

    def _on_close(self):
        self._disconnect()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    MoshIHM(root)
    root.mainloop()
