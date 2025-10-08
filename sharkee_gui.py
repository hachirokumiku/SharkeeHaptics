import socket
import logging
import time
import threading
import queue
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
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
    "Foot_R":     "foot_r.local",
}

# VRChat OSC Addresses (MUST match your avatar's receiver parameters)
VRC_OSC_MAP = {
    "/avatar/parameters/Receiver_Head": "Head",
    "/avatar/parameters/Receiver_Chest": "Chest",
    "/avatar/parameters/Receiver_UpperArm_L": "UpperArm_L",
    "/avatar/parameters/Receiver_UpperArm_R": "UpperArm_R",
    "/avatar/parameters/Receiver_Hips": "Hips",
    "/avatar/parameters/Receiver_UpperLeg_L": "UpperLeg_L",
    "/avatar/parameters/Receiver_UpperLeg_R": "UpperLeg_R",
    "/avatar/parameters/Receiver_LowerLeg_L": "LowerLeg_L",
    "/avatar/parameters/Receiver_LowerLeg_R": "LowerLeg_R",
    "/avatar/parameters/Receiver_Foot_L": "Foot_L",
    "/avatar/parameters/Receiver_Foot_R": "Foot_R",
}
# -------------------------------------

# --- THREAD SYNCHRONIZATION ---
# CRITICAL: This lock must be acquired before reading or writing any global state.
CACHE_LOCK = threading.Lock()

# Global state trackers (shared between threads)
GLOBAL_IP_OVERRIDES = {}
CLIENT_ONLINE_STATUS = {name: False for name in CLIENT_MAP.keys()} 
LAST_INTENSITY = {name: 0.0 for name in CLIENT_MAP.keys()} 
IP_CACHE = {} # Cache for resolved IPs (Hostname -> {'ip': 'x.x.x.x', 'timestamp': 12345})

# Performance Counters
PACKETS_RECEIVED = 0
PACKETS_ROUTED = 0

# Thread-safe queues
GUI_QUEUE = queue.Queue()       # For updating the GUI from background threads
RESOLUTION_QUEUE = queue.Queue()  # For requesting hostname lookups

# Global OSC Sender instance
CLIENT_SENDER = udp_client.SimpleUDPClient("127.0.0.1", INTERNAL_OSC_PORT)

# Track last received OSC message
LAST_RECEIVED_OSC = {"address": "None", "value": 0.0}


class HostnameResolver(threading.Thread):
    """
    Dedicated thread to handle slow, blocking network lookups (mDNS resolution), 
    preventing the OSC thread from freezing.
    """
    def __init__(self, gui_queue, ip_cache, client_map, cache_timeout, cache_lock, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.gui_queue = gui_queue
        self.ip_cache = ip_cache
        self.client_map = client_map
        self.cache_timeout = cache_timeout
        self.cache_lock = cache_lock
        self.daemon = True
        self.stop_event = threading.Event()

    def run(self):
        self.gui_queue.put({'type': 'LOG', 'message': "Resolver thread started.", 'level': 'INFO'})
        while not self.stop_event.is_set():
            try:
                # Check queue for urgent, non-cached resolution requests
                hostname = RESOLUTION_QUEUE.get(timeout=0.5)
                self._attempt_resolution(hostname, is_periodic=False)
                RESOLUTION_QUEUE.task_done()
            except queue.Empty:
                # Periodically check all known hosts for cache expiration
                self._periodic_check()
                
    def _attempt_resolution(self, hostname, is_periodic):
        """Attempts the blocking socket call to resolve hostname, protecting cache access."""
        try:
            # --- CRITICAL BLOCKING CALL RUNNING IN THIS DEDICATED THREAD ---
            ip = socket.gethostbyname(hostname) 
            # -----------------------------------------------------------------
            
            with self.cache_lock:
                cached_data = self.ip_cache.get(hostname, {})
                if ip != cached_data.get('ip'):
                    # IP resolved successfully or changed - WRITE ACCESS PROTECTED
                    self.ip_cache[hostname] = {'ip': ip, 'timestamp': time.time()}
                    
                    # Find the friendly receiver name
                    receiver_name = next(name for name, h in self.client_map.items() if h == hostname)
                    log_msg = f"RESOLVED: {hostname} -> {ip}"
                    self.gui_queue.put({'type': 'LOG', 'message': log_msg, 'level': 'SUCCESS'})
                    
                    self.gui_queue.put({
                        'type': 'RESOLVE_UPDATE',
                        'receiver': receiver_name,
                        'ip': ip,
                    })
        except socket.gaierror:
            # Resolution failed - Suppressing the log message as the main GUI table handles the status (OFFLINE).
            with self.cache_lock:
                if hostname in self.ip_cache:
                    # Mark cache entry invalid to force re-check later
                    self.ip_cache[hostname]['timestamp'] = 0 
            
            # Removed logging of resolution failure here to reduce log spam.
            # The status table update handles visibility of offline clients.


    def _periodic_check(self):
        """Checks if any cached IPs have expired and re-resolves them."""
        current_time = time.time()
        for hostname in self.client_map.values():
            
            # READ ACCESS PROTECTED
            with self.cache_lock:
                cached_data = self.ip_cache.get(hostname)

            # If not in cache, or if cache expired
            if not cached_data or (current_time - cached_data.get('timestamp', 0) > self.cache_timeout):
                self._attempt_resolution(hostname, is_periodic=True)
                time.sleep(0.1) 

    def stop(self):
        """Signals the thread to stop gracefully."""
        self.stop_event.set()


def sharkeehaptics_router_handler(address, *args):
    """
    NON-BLOCKING OSC Handler. Reads VRChat packets, applies rate-limit, 
    and uses CACHED IPs only. Requests async resolution if IP is missing.
    """
    
    global PACKETS_RECEIVED, PACKETS_ROUTED, LAST_RECEIVED_OSC
    PACKETS_RECEIVED += 1
    
    receiver_name = VRC_OSC_MAP.get(address)
    if not receiver_name:
        return
    
    # Track last received OSC message for GUI display
    current_intensity = float(args[0])
    LAST_RECEIVED_OSC = {"address": address, "value": current_intensity}
    
    # Notify GUI to update last message display
    GUI_QUEUE.put({
        'type': 'LAST_MESSAGE_UPDATE',
        'address': address,
        'value': current_intensity
    })
    
    target_hostname = CLIENT_MAP.get(receiver_name)

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
                if target_hostname and target_hostname not in RESOLUTION_QUEUE.queue:
                    RESOLUTION_QUEUE.put(target_hostname) 

    # 2. Check for Rate Limiting / Debouncing (LAG REDUCTION)
    with CACHE_LOCK:
        last_val = LAST_INTENSITY.get(receiver_name, 0.0)

    # Send if: 
    # a) Intensity changed significantly (abs(current - last) > threshold) 
    # OR b) It's a stop command (current_intensity < threshold/2 to ensure fast stops)
    should_send = (abs(current_intensity - last_val) > INTENSITY_THRESHOLD) or (current_intensity < INTENSITY_THRESHOLD/2)
    
    # 3. Queue performance counter update
    GUI_QUEUE.put({'type': 'COUNTER_UPDATE', 'received': PACKETS_RECEIVED, 'routed': PACKETS_ROUTED})

    # 4. Handle Routing
    if target_ip:
        # Client is connected (via cache or manual override)
        global CLIENT_ONLINE_STATUS
        CLIENT_ONLINE_STATUS[receiver_name] = True
        status_label = "MANUAL" if is_manual else "ONLINE"
        
        GUI_QUEUE.put({
            'type': 'STATUS_UPDATE', 
            'receiver': receiver_name, 
            'ip': target_ip, 
            'intensity': current_intensity,
            'status': status_label
        })

        if should_send:
            try:
                CLIENT_SENDER._address = target_ip
                CLIENT_SENDER._port = INTERNAL_OSC_PORT
                CLIENT_SENDER.send_message(INTERNAL_OSC_ADDRESS, current_intensity) 
                
                # WRITE ACCESS PROTECTED
                with CACHE_LOCK:
                    LAST_INTENSITY[receiver_name] = current_intensity
                
                PACKETS_ROUTED += 1

                if current_intensity > 0.05:
                    log_type = "MANUAL ROUTE" if is_manual else "ROUTE"
                    log_msg = f"[{log_type}] {receiver_name}: {current_intensity:.2f} -> {target_ip}"
                    GUI_QUEUE.put({'type': 'LOG', 'message': log_msg, 'level': 'INFO'})
                
            except Exception as e:
                # This handles send failures (e.g., client went offline after resolution)
                log_msg = f"Send failed to {target_hostname} ({target_ip}): {e}"
                GUI_QUEUE.put({'type': 'LOG', 'message': log_msg, 'level': 'ERROR'})
            
    else:
        # Client is offline/unresolved/expired in cache
        current_status = CLIENT_ONLINE_STATUS.get(receiver_name, False)
        if current_status:
            log_msg = f"Client '{receiver_name}' ({target_hostname}) status lost. Awaiting async resolution."
            GUI_QUEUE.put({'type': 'LOG', 'message': log_msg, 'level': 'WARN'})
            CLIENT_ONLINE_STATUS[receiver_name] = False

        GUI_QUEUE.put({
            'type': 'STATUS_UPDATE', 
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
        self.geometry("800x600")
        self.server_thread = None
        self.resolver_thread = None
        self.server = None
        self.is_running = False
        
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(3, weight=1)  # Status table row
        self.grid_rowconfigure(4, weight=1)  # Activity log row
        
        # Style Configuration
        self.style = ttk.Style(self)
        self.style.theme_use('clam')
        BG_DARK = '#1F2937' 
        BG_LIGHT = '#374151' 
        ACCENT_GREEN = '#10B981' 
        ACCENT_CYAN = '#06B6D4' 
        self.style.configure('TFrame', background=BG_DARK)
        self.style.configure('TLabel', background=BG_DARK, foreground='#E5E7EB', font=('Inter', 10))
        self.style.configure('Header.TLabel', background=BG_DARK, foreground=ACCENT_GREEN, font=('Inter', 18, 'bold'))
        self.style.configure('SubHeader.TLabel', background=BG_DARK, foreground=ACCENT_CYAN, font=('Inter', 12, 'bold'))
        self.style.configure('TButton', font=('Inter', 10, 'bold'), padding=8, background=BG_LIGHT, foreground='#E5E7EB')
        self.style.map('TButton', background=[('active', '#4B5563')])
        self.style.configure('Treeview', background=BG_LIGHT, foreground='#E5E7EB', fieldbackground=BG_LIGHT, bordercolor=BG_DARK, rowheight=28)
        self.style.map('Treeview', background=[('selected', ACCENT_GREEN)])
        self.style.configure('Treeview.Heading', font=('Inter', 10, 'bold'), background='#4B5563', foreground='#FFFFFF', padding=(5, 8))
        self.configure(background=BG_DARK)
        
        self.client_status = {
            name: {"ip": "---", "intensity": 0.0, "status": "UNKNOWN"}
            for name in CLIENT_MAP.keys()
        }
        
        self._create_widgets()
        self._setup_context_menu()
        self._start_gui_update_loop()
        
        self.toggle_server() 

    def _reset_counters(self):
        global PACKETS_RECEIVED, PACKETS_ROUTED
        PACKETS_RECEIVED = 0
        PACKETS_ROUTED = 0
        self.received_label.config(text="Received: 0")
        self.routed_label.config(text="Routed: 0")

    def _create_widgets(self):
        # Row 0: Header and Controls - with modular frame border
        header_frame = tk.LabelFrame(self, text="  Control Panel  ", bg='#1F2937', fg='#10B981', 
                                      font=('Inter', 11, 'bold'), relief=tk.RIDGE, bd=3,
                                      labelanchor='n', padx=15, pady=15)
        header_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=5)
        
        # Title Section
        title_label = ttk.Label(header_frame, text="SharkeeHaptics Router", style='Header.TLabel')
        title_label.pack(side=tk.LEFT)

        # Metrics Section
        metrics_frame = ttk.Frame(header_frame, style='TFrame')
        metrics_frame.pack(side=tk.RIGHT, padx=20)
        self.received_label = ttk.Label(metrics_frame, text="Received: 0", foreground='#FFEB3B', font=('Inter', 10, 'bold'))
        self.received_label.pack(side=tk.LEFT, padx=5)
        self.routed_label = ttk.Label(metrics_frame, text="Routed: 0", foreground='#4CAF50', font=('Inter', 10, 'bold'))
        self.routed_label.pack(side=tk.LEFT, padx=5)
        
        # Status and Toggle
        self.status_label = ttk.Label(header_frame, text="STATUS: STOPPED", font=('Inter', 12, 'bold'), foreground='#FF5722')
        self.status_label.pack(side=tk.RIGHT, padx=20)
        self.toggle_button = ttk.Button(header_frame, text="Start Router", command=self.toggle_server, style='TButton')
        self.toggle_button.pack(side=tk.RIGHT)
        
        # Row 0.5: Last Received OSC Message Display - with modular frame border
        last_msg_frame = tk.LabelFrame(self, text="  Last Received OSC Message  ", bg='#1F2937', fg='#06B6D4',
                                        font=('Inter', 10, 'bold'), relief=tk.RIDGE, bd=3,
                                        labelanchor='n', padx=10, pady=8)
        last_msg_frame.grid(row=1, column=0, sticky="ew", padx=10, pady=5)
        
        self.last_msg_label = ttk.Label(last_msg_frame, text="Address: None | Value: 0.00", 
                                         foreground='#FBBF24', font=('Inter', 10, 'bold'))
        self.last_msg_label.pack(anchor='w', padx=5)
        
        # Row 0.75: Action Buttons - with modular frame border
        buttons_frame = tk.LabelFrame(self, text="  Actions  ", bg='#1F2937', fg='#10B981',
                                       font=('Inter', 10, 'bold'), relief=tk.RIDGE, bd=3,
                                       labelanchor='n', padx=10, pady=10)
        buttons_frame.grid(row=2, column=0, sticky="ew", padx=10, pady=5)
        
        ttk.Button(buttons_frame, text="Test All Clients", command=self._test_all_clients, style='TButton').pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_frame, text="Refresh Clients", command=self._refresh_all_clients, style='TButton').pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_frame, text="Clear Log", command=self._clear_log, style='TButton').pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_frame, text="Help/About", command=self._show_help_about, style='TButton').pack(side=tk.LEFT, padx=5)
        
        # Row 3: Status Table - with modular frame border
        status_frame = tk.LabelFrame(self, text="  Client Connection Status  ", bg='#1F2937', fg='#06B6D4',
                                      font=('Inter', 11, 'bold'), relief=tk.RIDGE, bd=3,
                                      labelanchor='n', padx=10, pady=10)
        status_frame.grid(row=3, column=0, sticky="nsew", padx=10, pady=5)
        status_frame.grid_columnconfigure(0, weight=1)
        status_frame.grid_rowconfigure(0, weight=1)

        columns = ("vrc_receiver", "mdns_hostname", "resolved_ip", "intensity", "status")
        self.tree = ttk.Treeview(status_frame, columns=columns, show="headings", height=12) 
        self.tree.heading("vrc_receiver", text="VRC Receiver", anchor=tk.W)
        self.tree.heading("mdns_hostname", text="mDNS Hostname", anchor=tk.W)
        self.tree.heading("resolved_ip", text="Resolved IP (Right-Click to Manually Set)", anchor=tk.W)
        self.tree.heading("intensity", text="Intensity", anchor=tk.CENTER)
        self.tree.heading("status", text="Status", anchor=tk.CENTER)
        self.tree.column("vrc_receiver", width=150, anchor=tk.W); self.tree.column("mdns_hostname", width=150, anchor=tk.W);
        self.tree.column("resolved_ip", width=250, anchor=tk.W); self.tree.column("intensity", width=80, anchor=tk.CENTER);
        self.tree.column("status", width=120, anchor=tk.CENTER);
        self.tree.pack(fill='both', expand=True)
        
        self.tree_ids = {}
        for receiver, hostname in CLIENT_MAP.items():
            item_id = self.tree.insert("", tk.END, values=(receiver, hostname, "---", "0.00", "UNKNOWN"))
            self.tree_ids[receiver] = item_id

        # Row 4: Activity Log - with modular frame border
        log_frame = tk.LabelFrame(self, text="  Activity Log  ", bg='#1F2937', fg='#06B6D4',
                                   font=('Inter', 11, 'bold'), relief=tk.RIDGE, bd=3,
                                   labelanchor='n', padx=10, pady=10)
        log_frame.grid(row=4, column=0, sticky="nsew", padx=10, pady=5)
        log_frame.grid_columnconfigure(0, weight=1); log_frame.grid_rowconfigure(0, weight=1);

        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD, bg='#000000', fg='#FFFFFF', font=('Consolas', 9), height=10, border=0)
        self.log_text.pack(fill='both', expand=True)
        for level, color in self.LOG_COLORS.items():
            self.log_text.tag_configure(level, foreground=color)
        
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

    # --- New Action Button Methods ---
    
    def _test_all_clients(self):
        """Send a test signal to all clients (stub implementation)."""
        self.log_to_gui("TEST ALL: Sending test signal to all clients...", 'INFO')
        
        test_intensity = 0.5  # 50% intensity test signal
        
        for receiver_name, hostname in CLIENT_MAP.items():
            # Get the IP for each client
            with CACHE_LOCK:
                target_ip = GLOBAL_IP_OVERRIDES.get(receiver_name)
                if not target_ip:
                    cached_data = IP_CACHE.get(hostname)
                    if cached_data:
                        target_ip = cached_data.get('ip')
            
            if target_ip:
                try:
                    CLIENT_SENDER._address = target_ip
                    CLIENT_SENDER._port = INTERNAL_OSC_PORT
                    CLIENT_SENDER.send_message(INTERNAL_OSC_ADDRESS, test_intensity)
                    self.log_to_gui(f"TEST: Sent {test_intensity:.2f} to {receiver_name} ({target_ip})", 'SUCCESS')
                except Exception as e:
                    self.log_to_gui(f"TEST FAILED: {receiver_name} ({target_ip}): {e}", 'ERROR')
            else:
                self.log_to_gui(f"TEST SKIPPED: {receiver_name} - No IP available", 'WARN')
        
        self.log_to_gui("TEST ALL: Complete", 'INFO')
    
    def _refresh_all_clients(self):
        """Re-resolve all client IPs by clearing cache and requesting resolution."""
        self.log_to_gui("REFRESH: Re-resolving all client IPs...", 'INFO')
        
        with CACHE_LOCK:
            # Clear the IP cache to force re-resolution
            IP_CACHE.clear()
        
        # Queue all hostnames for resolution
        for hostname in CLIENT_MAP.values():
            if hostname not in RESOLUTION_QUEUE.queue:
                RESOLUTION_QUEUE.put(hostname)
        
        self.log_to_gui("REFRESH: All clients queued for IP resolution", 'SUCCESS')
    
    def _clear_log(self):
        """Clear the activity log."""
        self.log_text.config(state='normal')
        self.log_text.delete('1.0', tk.END)
        self.log_text.config(state='disabled')
        self.log_to_gui("Activity log cleared", 'INFO')
    
    def _show_help_about(self):
        """Show help/about dialog."""
        help_text = """SharkeeHaptics Router
        
A haptic routing system for VRChat OSC messages.

Usage:
1. Click "Start Router" to begin listening for VRChat OSC messages
2. The router will automatically discover haptic clients via mDNS
3. Right-click on any client to manually set its IP address
4. Monitor client status and activity in real-time

Features:
• Test All: Send a test signal to all connected clients
• Refresh Clients: Re-resolve all client IP addresses
• Clear Log: Clear the activity log display
• Auto-discovery: Clients are automatically discovered via mDNS

Configuration:
• VRChat OSC Listen Port: 9001
• Client OSC Port: 8000
• mDNS Cache Timeout: 10 minutes

For more information, visit the GitHub repository.
"""
        messagebox.showinfo("SharkeeHaptics Router - Help", help_text)

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

        ttk.Button(popup, text="Apply", command=on_submit).pack(pady=10); popup.bind("<Return>", lambda event: on_submit())

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
            self.log_to_gui(f"OSC Server started on port {VRC_OSC_LISTEN_PORT}.", level='INFO')
            self.toggle_button.config(text="Stop Router")
            
        except OSError as e:
            msg = f"Failed to start OSC server (Port {VRC_OSC_LISTEN_PORT} likely in use): {e}"
            self._update_gui_status("Failed", '#FF0000')
            self.log_to_gui(msg, 'ERROR')
            self.is_running = False
            self.toggle_button.config(text="Start Router")

    def _stop_server_in_thread(self):
        # 1. Stop Resolver Thread
        if self.resolver_thread:
            self.resolver_thread.stop()
            self.resolver_thread.join(timeout=1.0)
            self.resolver_thread = None

        # 2. Stop OSC Server Thread
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server_thread.join(timeout=1.0)
            self.server = None
            self.server_thread = None
            self.is_running = False
            
            # Reset states
            with CACHE_LOCK:
                self.client_status = {name: {"ip": GLOBAL_IP_OVERRIDES.get(name, "---"), "intensity": 0.0, "status": "STOPPED"} for name in CLIENT_MAP.keys()}
                global CLIENT_ONLINE_STATUS
                CLIENT_ONLINE_STATUS = {name: False for name in CLIENT_MAP.keys()}
            
            self._update_client_table_all()
            self._reset_counters()

            self._update_gui_status("Stopped", '#FF5722')
            self.log_to_gui("Router service stopped successfully.", level='INFO')
            self.toggle_button.config(text="Start Router")

    def toggle_server(self):
        if self.is_running:
            self._stop_server_in_thread()
        else:
            self._start_server_in_thread()

    # --- GUI Update Logic ---
    def _start_gui_update_loop(self):
        self.after(100, self._process_gui_queue)

    def _process_gui_queue(self):
        while not GUI_QUEUE.empty():
            try:
                data = GUI_QUEUE.get_nowait()
                if data['type'] == 'LOG':
                    self.log_to_gui(data['message'], data.get('level', 'INFO'))
                elif data['type'] == 'STATUS_UPDATE':
                    self._update_client_status(data)
                elif data['type'] == 'RESOLVE_UPDATE':
                    self.log_to_gui(f"IP updated for {data['receiver']} to {data['ip']}", 'SUCCESS')
                    self.client_status[data['receiver']].update({"ip": data['ip'], "status": "ONLINE"})
                    self._update_client_table_all()
                elif data['type'] == 'COUNTER_UPDATE':
                    self.received_label.config(text=f"Received: {data['received']}")
                    self.routed_label.config(text=f"Routed: {data['routed']}")
                elif data['type'] == 'LAST_MESSAGE_UPDATE':
                    # Update the last received OSC message display
                    address = data['address']
                    value = data['value']
                    self.last_msg_label.config(text=f"Address: {address} | Value: {value:.2f}")
            except queue.Empty:
                break
            except Exception as e:
                self.log_to_gui(f"FATAL GUI ERROR during queue processing: {e}", 'ERROR') 
        
        self.after(100, self._process_gui_queue)

    def _update_gui_status(self, text, color):
        self.status_label.config(text=f"STATUS: {text}", foreground=color)
    
    def log_to_gui(self, message, level='INFO'):
        timestamp = time.strftime("[%H:%M:%S]")
        level_prefix = f"[{level}] " if level != 'INFO' else ""
        formatted_message = f"{timestamp} {level_prefix}{message}\n"
        
        self.log_text.config(state='normal')
        self.log_text.insert(tk.END, formatted_message, (level,))
        self.log_text.see(tk.END)
        self.log_text.config(state='disabled')

    def _update_client_status(self, data):
        receiver = data['receiver']
        
        self.client_status[receiver].update({
            "ip": data['ip'],
            "intensity": data['intensity'],
            "status": data['status']
        })
        
        item_id = self.tree_ids.get(receiver)
        if item_id:
            intensity_str = f"{data['intensity']:.2f}"
            self.tree.item(item_id, values=(receiver, CLIENT_MAP.get(receiver), data['ip'], intensity_str, data['status']))
            
            new_tags = ['base']
            self.tree.tag_configure('base', foreground='#E5E7EB', background='#374151', font=('Inter', 10, 'normal'))
            
            if data['status'] == 'MANUAL': new_tags.append('manual'); self.tree.tag_configure('manual', foreground='#06B6D4')
            elif data['status'] in ('OFFLINE', 'STOPPED', 'UNKNOWN'): new_tags.append('offline'); self.tree.tag_configure('offline', foreground='#FF5722')
            
            is_connected = data['status'] in ('ONLINE', 'MANUAL')
            if is_connected and data['intensity'] > 0.05:
                new_tags.append('active')
                self.tree.tag_configure('active', background='#4B5563', foreground='#FBBF24', font=('Inter', 10, 'bold'))
            
            self.tree.item(item_id, tags=new_tags)
    
    def _update_client_table_all(self):
        for receiver, status_data in self.client_status.items():
            self._update_client_status({
                'receiver': receiver, 'ip': status_data['ip'], 
                'intensity': status_data['intensity'], 'status': status_data['status']
            })
            
    def on_closing(self):
        self.log_to_gui("Application closing...", level='INFO')
        self._stop_server_in_thread()
        self.destroy()


if __name__ == "__main__":
    app = SharkeeHapticsRouterApp()
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()
