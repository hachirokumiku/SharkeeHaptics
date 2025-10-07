import socket
import logging
import time
import threading
import queue
import tkinter as tk
from tkinter import ttk, scrolledtext
from pythonosc import dispatcher
from pythonosc import osc_server
from pythonosc import udp_client

# --- Configuration for Maximum Lag Reduction and Stability ---

# VRChat sends OSC signals to this address/port (usually localhost:9001)
VRC_OSC_LISTEN_IP = "0.0.0.0" 
VRC_OSC_LISTEN_PORT = 9001 

# The standardized OSC message the router sends to the clients
INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity"
INTERNAL_OSC_PORT = 8000 

# Aggressive threshold for rate-limiting. (0.03 = 3%)
INTENSITY_THRESHOLD = 0.03

# Increased mDNS cache lifespan (in seconds). 
CACHE_TIMEOUT_SECONDS = 600 # 10 minutes

# --- CLIENT MAPPING (Maps VRChat Receiver Name to mDNS Hostname) ---
CLIENT_MAP = {
    "Head":       "head.local",
    "Chest":      "chest.local", 
    "UpperArm_L": "upperarm_l.local",
    "UpperArm_R": "upperarm_r.local",
    "Hips":       "hips.local",
    "UpperLeg_L": "upperleg_l.local",
    "UpperLeg_R": "upperleg_r.local",
    "LowerLeg_L": "lowerleg_l.local",
    "LowerLeg_R": "lowerleg_r.local",
    "Foot_L":     "foot_l.local",
    "Foot_R":     "foot_r.local"
}

# VRChat OSC Address Mapping (VRChat address -> Receiver Name)
VRC_OSC_MAP = {
    "/avatar/parameters/SharkeeHead":      "Head",
    "/avatar/parameters/SharkeeChest":     "Chest",
    "/avatar/parameters/SharkeeUpperArm_L": "UpperArm_L",
    "/avatar/parameters/SharkeeUpperArm_R": "UpperArm_R",
    "/avatar/parameters/SharkeeHips":      "Hips",
    "/avatar/parameters/SharkeeUpperLeg_L": "UpperLeg_L",
    "/avatar/parameters/SharkeeUpperLeg_R": "UpperLeg_R",
    "/avatar/parameters/SharkeeLowerLeg_L": "LowerLeg_L",
    "/avatar/parameters/SharkeeLowerLeg_R": "LowerLeg_R",
    "/avatar/parameters/SharkeeFoot_L":    "Foot_L",
    "/avatar/parameters/SharkeeFoot_R":    "Foot_R"
}

# --- GLOBAL STATE ---
IP_CACHE = {}               # {hostname: {'ip': str, 'timestamp': float}}
GLOBAL_IP_OVERRIDES = {}    # {receiver_name: ip_address} for manual overrides
CACHE_LOCK = threading.Lock()
PACKETS_RECEIVED = 0
PACKETS_ROUTED = 0
LAST_INTENSITY = {}         # {receiver_name: last_intensity}

GUI_QUEUE = queue.Queue()       # For updating the GUI from background threads
RESOLUTION_QUEUE = queue.Queue()  # For requesting hostname lookups

# Global OSC Sender instance
CLIENT_SENDER = udp_client.SimpleUDPClient("127.0.0.1", INTERNAL_OSC_PORT)


class HostnameResolver(threading.Thread):
    """
    Background thread that resolves hostnames asynchronously without blocking the OSC handler.
    """
    def __init__(self, gui_queue, ip_cache, client_map, cache_timeout, cache_lock):
        super().__init__(daemon=True)
        self.gui_queue = gui_queue
        self.ip_cache = ip_cache
        self.client_map = client_map
        self.cache_timeout = cache_timeout
        self.cache_lock = cache_lock
        self.running = True
        
    def run(self):
        while self.running:
            try:
                # Wait for a hostname resolution request (with timeout to allow checking self.running)
                try:
                    hostname = RESOLUTION_QUEUE.get(timeout=1.0)
                except queue.Empty:
                    continue
                    
                # Perform blocking DNS resolution
                try:
                    resolved_ip = socket.gethostbyname(hostname)
                    
                    # Update cache with lock
                    with self.cache_lock:
                        self.ip_cache[hostname] = {
                            'ip': resolved_ip,
                            'timestamp': time.time()
                        }
                    
                    # Notify GUI
                    self.gui_queue.put({
                        'type': 'log',
                        'message': f"Resolved {hostname} -> {resolved_ip}",
                        'level': 'INFO'
                    })
                    
                except socket.gaierror:
                    self.gui_queue.put({
                        'type': 'log',
                        'message': f"Failed to resolve {hostname}",
                        'level': 'WARN'
                    })
                    
            except Exception as e:
                self.gui_queue.put({
                    'type': 'log',
                    'message': f"Resolver error: {e}",
                    'level': 'ERROR'
                })
    
    def stop(self):
        self.running = False


def sharkeehaptics_router_handler(address, *args):
    """
    NON-BLOCKING OSC Handler. Reads VRChat packets, applies rate-limit, 
    and uses CACHED IPs only. Requests async resolution if IP is missing.
    """
    
    global PACKETS_RECEIVED, PACKETS_ROUTED
    PACKETS_RECEIVED += 1
    
    receiver_name = VRC_OSC_MAP.get(address)
    if not receiver_name:
        return
    
    target_hostname = CLIENT_MAP.get(receiver_name)
    current_intensity = float(args[0])

    # 1. Determine Target IP (MUST BE NON-BLOCKING)
    with CACHE_LOCK:
        target_ip = GLOBAL_IP_OVERRIDES.get(receiver_name)
        is_manual = bool(target_ip)
        
        if not target_ip:
            cached_data = IP_CACHE.get(target_hostname)
            current_time = time.time()
            
            # Check cache validity
            if cached_data and (current_time - cached_data['timestamp'] < CACHE_TIMEOUT_SECONDS):
                target_ip = cached_data['ip']
            else:
                # IP is missing or expired -> Queue for ASYNC resolution
                RESOLUTION_QUEUE.put(target_hostname)
                
                # Update GUI status
                GUI_QUEUE.put({
                    'type': 'client_status',
                    'receiver': receiver_name,
                    'ip': '---',
                    'intensity': current_intensity,
                    'status': 'RESOLVING'
                })
                return

    # 2. Rate Limiting (skip if intensity change is too small)
    last_intensity = LAST_INTENSITY.get(receiver_name, 0.0)
    if abs(current_intensity - last_intensity) < INTENSITY_THRESHOLD:
        return
    
    LAST_INTENSITY[receiver_name] = current_intensity

    # 3. Send OSC Message (NON-BLOCKING)
    if target_ip:
        try:
            client = udp_client.SimpleUDPClient(target_ip, INTERNAL_OSC_PORT)
            client.send_message(INTERNAL_OSC_ADDRESS, current_intensity)
            PACKETS_ROUTED += 1
            
            status = 'MANUAL' if is_manual else 'ONLINE'
            GUI_QUEUE.put({
                'type': 'client_status',
                'receiver': receiver_name,
                'ip': target_ip,
                'intensity': current_intensity,
                'status': status
            })
        except Exception as e:
            GUI_QUEUE.put({
                'type': 'log',
                'message': f"Error sending to {receiver_name}: {e}",
                'level': 'ERROR'
            })
            GUI_QUEUE.put({
                'type': 'client_status',
                'receiver': receiver_name,
                'ip': target_ip,
                'intensity': current_intensity,
                'status': 'ERROR'
            })
    else:
        GUI_QUEUE.put({
            'type': 'client_status',
            'receiver': receiver_name,
            'ip': '---',
            'intensity': current_intensity,
            'status': 'OFFLINE'
        })


class SharkeeHapticsRouterApp(tk.Tk):
    """Main application class for the SharkeeHaptics Haptic Router GUI."""
    
    LOG_COLORS = {
        'INFO': '#C0C0C0', 'WARN': '#FFEB3B', 'ERROR': '#FF5722', 'SUCCESS': '#4CAF50'
    }
    
    def __init__(self):
        super().__init__()
        self.title("SharkeeHaptics Router")
        self.geometry("900x700")
        self.configure(background='#1E1E1E')
        
        self.is_running = False
        self.server = None
        self.server_thread = None
        self.resolver_thread = None
        
        # Initialize client status tracking
        self.client_status = {}
        for receiver_name in CLIENT_MAP.keys():
            self.client_status[receiver_name] = {
                'ip': '---',
                'intensity': 0.0,
                'status': 'OFFLINE'
            }
        
        self._create_widgets()
        self._setup_context_menu()
        self._start_gui_update_loop()
    
    def _reset_counters(self):
        global PACKETS_RECEIVED, PACKETS_ROUTED
        PACKETS_RECEIVED = 0
        PACKETS_ROUTED = 0
    
    def _create_widgets(self):
        # Row 0: Header and Controls
        header_frame = ttk.Frame(self, style='TFrame', padding="15 15")
        header_frame.grid(row=0, column=0, sticky="ew")
        title_label = ttk.Label(header_frame, text="SharkeeHaptics Router", style='Header.TLabel')
        title_label.pack(side=tk.LEFT)

        metrics_frame = ttk.Frame(header_frame, style='TFrame')
        metrics_frame.pack(side=tk.RIGHT, padx=20)
        self.received_label = ttk.Label(metrics_frame, text="Received: 0", foreground='#FFEB3B', font=('Inter', 10, 'bold'))
        self.received_label.pack(side=tk.LEFT, padx=5)
        self.routed_label = ttk.Label(metrics_frame, text="Routed: 0", foreground='#4CAF50', font=('Inter', 10, 'bold'))
        self.routed_label.pack(side=tk.LEFT, padx=5)

        # Row 1: Status and Toggle Button
        status_frame = ttk.Frame(self, style='TFrame', padding="0 10")
        status_frame.grid(row=1, column=0, sticky="ew")
        self.status_label = ttk.Label(status_frame, text="Status: Stopped", font=('Inter', 12, 'bold'), foreground='#FF5722')
        self.status_label.pack(side=tk.LEFT, padx=15)
        self.toggle_button = tk.Button(status_frame, text="Start Server", command=self.toggle_server, 
                                        bg='#4CAF50', fg='white', font=('Inter', 10, 'bold'), 
                                        relief=tk.FLAT, padx=20, pady=5)
        self.toggle_button.pack(side=tk.LEFT, padx=10)

        # Row 2: Client Status Table
        table_frame = ttk.Frame(self, style='TFrame', padding="15 5")
        table_frame.grid(row=2, column=0, sticky="nsew", padx=15, pady=5)
        
        table_label = ttk.Label(table_frame, text="Client Status", font=('Inter', 11, 'bold'), foreground='#00D4FF')
        table_label.pack(anchor="w", pady=5)
        
        self.tree = ttk.Treeview(table_frame, columns=("Receiver", "IP", "Intensity", "Status"), 
                                 show="headings", height=11)
        self.tree.heading("Receiver", text="Receiver")
        self.tree.heading("IP", text="IP Address")
        self.tree.heading("Intensity", text="Intensity")
        self.tree.heading("Status", text="Status")
        
        self.tree.column("Receiver", width=150)
        self.tree.column("IP", width=200)
        self.tree.column("Intensity", width=100)
        self.tree.column("Status", width=150)
        
        self.tree.pack(fill=tk.BOTH, expand=True)
        
        # Populate initial table
        for receiver_name in CLIENT_MAP.keys():
            self.tree.insert("", tk.END, iid=receiver_name, values=(
                receiver_name, '---', '0.00', 'OFFLINE'
            ))

        # Row 3: Log Window
        log_frame = ttk.Frame(self, style='TFrame', padding="15 5")
        log_frame.grid(row=3, column=0, sticky="nsew", padx=15, pady=5)
        
        log_label = ttk.Label(log_frame, text="Activity Log", font=('Inter', 11, 'bold'), foreground='#00D4FF')
        log_label.pack(anchor="w", pady=5)
        
        self.log_text = scrolledtext.ScrolledText(log_frame, height=12, bg='#2D2D2D', fg='#FFFFFF', 
                                                   font=('Consolas', 9), wrap=tk.WORD, state=tk.DISABLED)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        
        # Configure row/column weights for resizing
        self.grid_rowconfigure(2, weight=2)
        self.grid_rowconfigure(3, weight=1)
        self.grid_columnconfigure(0, weight=1)
        
        # Configure styles
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TFrame', background='#1E1E1E')
        style.configure('Header.TLabel', background='#1E1E1E', foreground='#00D4FF', font=('Inter', 16, 'bold'))
        style.configure('TLabel', background='#1E1E1E', foreground='#FFFFFF')
        style.configure('Treeview', background='#2D2D2D', foreground='#FFFFFF', 
                       fieldbackground='#2D2D2D', borderwidth=0)
        style.configure('Treeview.Heading', background='#3A3A3A', foreground='#00D4FF', 
                       font=('Inter', 10, 'bold'))
        style.map('Treeview', background=[('selected', '#4A4A4A')])

    def _setup_context_menu(self):
        self.context_menu = tk.Menu(self, tearoff=0, background='#3A3A3A', foreground='#FFFFFF')
        self.context_menu.add_command(label="Set Manual IP", command=self._show_ip_entry_popup)
        self.context_menu.add_command(label="Clear Manual IP", command=lambda: self._set_manual_ip(clear=True))
        self.tree.bind("<Button-3>", self._show_context_menu)

    def _show_context_menu(self, event):
        item_id = self.tree.identify_row(event.y)
        if item_id:
            self.tree.selection_set(item_id) 
            self.tree.focus(item_id)
            try: self.context_menu.tk_popup(event.x_root, event.y_root)
            finally: self.context_menu.grab_release()
    
    # --- Manual IP Override/Clear Logic (Kept for addressing non-mDNS devices) ---
    def _show_ip_entry_popup(self):
        selected_items = self.tree.selection()
        if not selected_items: return
        item_id = selected_items[0]
        receiver_name = self.tree.item(item_id, 'values')[0]
        
        with CACHE_LOCK:
            current_ip = GLOBAL_IP_OVERRIDES.get(receiver_name, "")
        
        popup = tk.Toplevel(self)
        popup.title(f"Set Manual IP for {receiver_name}")
        popup.geometry("300x120"); popup.configure(background='#2D2D2D'); popup.attributes('-topmost', True); 
        ip_entry = ttk.Entry(popup, width=20, font=('Inter', 10)); ip_entry.insert(0, current_ip); ip_entry.pack(pady=5, padx=20); ip_entry.focus_set()

        def on_submit():
            new_ip = ip_entry.get().strip(); self._set_manual_ip(receiver_name=receiver_name, ip_address=new_ip); popup.destroy()
        
        ttk.Button(popup, text="Set IP", command=on_submit).pack(pady=5)

    def _set_manual_ip(self, receiver_name=None, ip_address=None, clear=False):
        if receiver_name is None:
            selected_items = self.tree.selection()
            if not selected_items: return
            receiver_name = self.tree.item(selected_items[0], 'values')[0]

        global GLOBAL_IP_OVERRIDES
        
        with CACHE_LOCK: # Protect global override access
            if clear:
                if receiver_name in GLOBAL_IP_OVERRIDES: del GLOBAL_IP_OVERRIDES[receiver_name]
                self.log_to_gui(f"Manual IP cleared for {receiver_name}. Now using mDNS.", level='INFO')
                self.client_status[receiver_name].update({"ip": "---", "status": "UNKNOWN"})
            elif ip_address:
                if ip_address.count('.') == 3 and all(p.isdigit() or p == '.' for p in ip_address):
                    GLOBAL_IP_OVERRIDES[receiver_name] = ip_address
                    self.log_to_gui(f"Manual IP set for {receiver_name} to {ip_address}", level='SUCCESS')
                    self.client_status[receiver_name].update({"ip": ip_address, "status": "MANUAL"})
                else:
                    self.log_to_gui(f"Invalid IP address format: {ip_address}", level='ERROR')
        
        self._update_client_table_all()
    
    # --- Server and Threading Logic ---

    def _start_server_in_thread(self):
        disp = dispatcher.Dispatcher()
        for vrc_address in VRC_OSC_MAP.keys():
            # Renamed handler
            disp.map(vrc_address, sharkeehaptics_router_handler)

        # 1. Start the ASYNC Resolver Thread, passing the lock
        self.resolver_thread = HostnameResolver(GUI_QUEUE, IP_CACHE, CLIENT_MAP, CACHE_TIMEOUT_SECONDS, CACHE_LOCK)
        self.resolver_thread.start()

        # 2. Start the OSC Server Thread
        try:
            self.server = osc_server.ThreadingOSCUDPServer((VRC_OSC_LISTEN_IP, VRC_OSC_LISTEN_PORT), disp)
            self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
            self.server_thread.start()
            self.is_running = True
            
            self._reset_counters()
            self._update_gui_status("Running", '#4CAF50')
            self.toggle_button.config(text="Stop Server", bg='#FF5722')
            self.log_to_gui(f"Router started. Listening on {VRC_OSC_LISTEN_IP}:{VRC_OSC_LISTEN_PORT}", level='SUCCESS')
        except Exception as e:
            self.log_to_gui(f"Failed to start server: {e}", level='ERROR')
            self._update_gui_status("Error", '#FF5722')

    def _stop_server_in_thread(self):
        if self.server:
            self.server.shutdown()
            self.server = None
        if self.resolver_thread:
            self.resolver_thread.stop()
            self.resolver_thread = None
        
        self.is_running = False
        self._update_gui_status("Stopped", '#FF5722')
        self.toggle_button.config(text="Start Server", bg='#4CAF50')
        self.log_to_gui("Router stopped.", level='INFO')

    def toggle_server(self):
        if self.is_running:
            self._stop_server_in_thread()
        else:
            self._start_server_in_thread()
    
    # --- GUI Update Logic ---
    def _update_gui_status(self, status_text, color):
        self.status_label.config(text=f"Status: {status_text}", foreground=color)
    
    def log_to_gui(self, message, level='INFO'):
        timestamp = time.strftime('%H:%M:%S')
        color = self.LOG_COLORS.get(level, '#FFFFFF')
        formatted_message = f"[{timestamp}] [{level}] {message}\n"
        
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, formatted_message, level)
        self.log_text.tag_config(level, foreground=color)
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)
    
    def _update_client_table(self, receiver_name, ip, intensity, status):
        self.client_status[receiver_name] = {
            'ip': ip,
            'intensity': intensity,
            'status': status
        }
        self.tree.item(receiver_name, values=(
            receiver_name, ip, f"{intensity:.2f}", status
        ))
    
    def _update_client_table_all(self):
        for receiver_name, data in self.client_status.items():
            self.tree.item(receiver_name, values=(
                receiver_name, data['ip'], f"{data['intensity']:.2f}", data['status']
            ))
    
    def _start_gui_update_loop(self):
        """Process messages from the GUI queue and update the interface."""
        try:
            while not GUI_QUEUE.empty():
                msg = GUI_QUEUE.get_nowait()
                
                if msg['type'] == 'log':
                    self.log_to_gui(msg['message'], level=msg['level'])
                elif msg['type'] == 'client_status':
                    self._update_client_table(
                        msg['receiver'], msg['ip'], msg['intensity'], msg['status']
                    )
        except queue.Empty:
            pass
        
        # Update metrics
        self.received_label.config(text=f"Received: {PACKETS_RECEIVED}")
        self.routed_label.config(text=f"Routed: {PACKETS_ROUTED}")
        
        # Schedule next update
        self.after(100, self._start_gui_update_loop)
    
    def on_closing(self):
        if self.is_running:
            self._stop_server_in_thread()
        self.destroy()


if __name__ == "__main__":
    app = SharkeeHapticsRouterApp()
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()
