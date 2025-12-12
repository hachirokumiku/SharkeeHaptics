import socket
import logging
import time
import threading
import queue
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
from pythonosc import dispatcher
from pythonosc import osc_server
# CRITICAL FIX: Need osc_message_builder to manually create the packet
from pythonosc import osc_message_builder 
from pythonosc import udp_client # Keep for server only

# --- Configuration for Maximum Lag Reduction and Stability ---

# VRChat sends OSC signals to this address/port (usually localhost:9001)
VRC_OSC_LISTEN_IP = "0.0.0.0" 
VRC_OSC_LISTEN_PORT = 9001 

# The router broadcasts to this IP on the subnet. 255.255.255.255 is the standard local broadcast address.
BROADCAST_IP = "255.255.255.255" 

# The base OSC message the router broadcasts. The receiver role will be appended.
INTERNAL_OSC_ADDRESS_BASE = "/sharkeehaptics/set_intensity"
INTERNAL_OSC_PORT = 8000 

# Aggressive threshold for rate-limiting. (0.03 = 3%)
INTENSITY_THRESHOLD = 0.03

# GUI update interval (ms) tuned for ~60 Hz refresh
GUI_UPDATE_INTERVAL_MS = 17 
MAX_GUI_ITEMS_PER_LOOP = 1000

# --- CLIENT MAPPING (Maps VRChat Receiver Name to mDNS Hostname) ---
# The keys here will be used in the broadcast address (converted to lowercase).
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

# --- GLOBAL STATE ---
# Use a custom socket to ensure broadcast is enabled
BROADCAST_SENDER_SOCKET = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
BROADCAST_SENDER_SOCKET.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

# Global state trackers (shared between threads)
LAST_INTENSITY = {name: 0.0 for name in CLIENT_MAP.keys()} 
CLIENT_ONLINE_STATUS = {name: False for name in CLIENT_MAP.keys()} 

# Performance Counters
PACKETS_RECEIVED = 0
PACKETS_ROUTED = 0

# Thread-safe queues
GUI_QUEUE = queue.Queue()       # For updating the GUI from background threads

# Track last received OSC message
LAST_RECEIVED_OSC = {"address": "None", "value": 0.0}

def send_osc_via_broadcast(address, value):
    """
    Encodes and sends a single float OSC message via the custom broadcast socket.
    
    This replaces SimpleUDPClient's send_message functionality for broadcast.
    """
    builder = osc_message_builder.OscMessageBuilder(address=address)
    # The 'f' type tag is added implicitly by adding a float
    builder.add_arg(value, "f") 
    msg = builder.build()
    # Send the raw datagram bytes (msg.dgram) to the broadcast address and port
    BROADCAST_SENDER_SOCKET.sendto(msg.dgram, (BROADCAST_IP, INTERNAL_OSC_PORT))


def sharkeehaptics_router_handler(address, *args):
    """
    NON-BLOCKING OSC Handler. Reads VRChat packets, applies rate-limit, 
    and broadcasts the message to all clients.
    """
    
    global PACKETS_RECEIVED, PACKETS_ROUTED, LAST_RECEIVED_OSC
    PACKETS_RECEIVED += 1
    
    # receiver_name is in Title-Case (e.g., "Head")
    receiver_name = VRC_OSC_MAP.get(address)
    if not receiver_name:
        return
    
    current_intensity = float(args[0])
    LAST_RECEIVED_OSC = {"address": address, "value": current_intensity}
    
    GUI_QUEUE.put({
        'type': 'LAST_MESSAGE_UPDATE',
        'address': address,
        'value': current_intensity
    })
    
    # CRITICAL CHANGE: Build the broadcast address using the lowercase receiver name
    client_receiver_name = receiver_name.lower()
    full_osc_address = f"{INTERNAL_OSC_ADDRESS_BASE}/{client_receiver_name}"
    
    # 1. Check for Rate Limiting / Debouncing (LAG REDUCTION)
    last_val = LAST_INTENSITY.get(receiver_name, 0.0)

    # Send if: Intensity changed significantly OR It's a stop command
    should_send = (abs(current_intensity - last_val) > INTENSITY_THRESHOLD) or (current_intensity < INTENSITY_THRESHOLD/2)
    
    # 2. Handle Routing (Broadcast)
    if should_send:
        try:
            # Use the new broadcast function to send the message
            send_osc_via_broadcast(full_osc_address, current_intensity) 
            
            LAST_INTENSITY[receiver_name] = current_intensity
            
            PACKETS_ROUTED += 1

            if current_intensity > 0.05:
                log_msg = f"[BROADCAST] {receiver_name}: {current_intensity:.2f} -> {BROADCAST_IP}:{INTERNAL_OSC_PORT} ({full_osc_address})"
                GUI_QUEUE.put({'type': 'LOG', 'message': log_msg, 'level': 'INFO'})
                
            # Update GUI status (assuming it is broadcasting)
            GUI_QUEUE.put({
                'type': 'STATUS_UPDATE', 
                'receiver': receiver_name, 
                'ip': BROADCAST_IP, 
                'intensity': current_intensity,
                'status': 'BROADCASTING'
            })
            
        except Exception as e:
            # Log only if sending fails entirely (e.g., firewall blocked)
            log_msg = f"Broadcast send failed to {BROADCAST_IP}:{INTERNAL_OSC_PORT}: {e}"
            GUI_QUEUE.put({'type': 'LOG', 'message': log_msg, 'level': 'ERROR'})

    # 3. Queue performance counter update
    GUI_QUEUE.put({'type': 'COUNTER_UPDATE', 'received': PACKETS_RECEIVED, 'routed': PACKETS_ROUTED})


class SharkeeHapticsRouterApp(tk.Tk):
    """Main application class for the SharkeeHaptics Haptic Router GUI."""
    
    LOG_COLORS = {
        'INFO': '#C0C0C0', 'WARN': '#FFEB3B', 'ERROR': '#FF5722', 'SUCCESS': '#4CAF50'
    }
    
    def __init__(self):
        super().__init__()
        self.title("SharkeeHaptics Broadcast Router")
        self.geometry("800x600")
        self.server_thread = None
        self.resolver_thread = None # No resolver thread in broadcast mode
        self.server = None
        self.is_running = False
        
        # FIX: Define theme variables here as instance variables
        self.BG_DARK = '#1F2937' 
        self.BG_LIGHT = '#374151' 
        self.ACCENT_GREEN = '#10B981' 
        self.ACCENT_CYAN = '#06B6D4' 
        
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(3, weight=1)  # Status table row
        self.grid_rowconfigure(4, weight=1)  # Activity log row
        
        # Style Configuration remains the same
        self.style = ttk.Style(self)
        self.style.theme_use('clam')
        
        self.style.configure('TFrame', background=self.BG_DARK)
        self.style.configure('TLabel', background=self.BG_DARK, foreground='#E5E7EB', font=('Inter', 10))
        self.style.configure('Header.TLabel', background=self.BG_DARK, foreground=self.ACCENT_GREEN, font=('Inter', 18, 'bold'))
        self.style.configure('SubHeader.TLabel', background=self.BG_DARK, foreground=self.ACCENT_CYAN, font=('Inter', 12, 'bold'))
        self.style.configure('TButton', font=('Inter', 10, 'bold'), padding=8, background=self.BG_LIGHT, foreground='#E5E7EB')
        self.style.map('TButton', background=[('active', '#4B5563')])
        self.style.configure('Treeview', background=self.BG_LIGHT, foreground='#E5E7EB', fieldbackground=self.BG_LIGHT, bordercolor=self.BG_DARK, rowheight=28)
        self.style.map('Treeview', background=[('selected', self.ACCENT_GREEN)])
        self.style.configure('Treeview.Heading', font=('Inter', 10, 'bold'), background='#4B5563', foreground='#FFFFFF', padding=(5, 8))
        self.configure(background=self.BG_DARK)
        
        # Client status is simplified, always showing the broadcast IP
        self.client_status = {
            name: {"ip": BROADCAST_IP, "intensity": 0.0, "status": "STOPPED"}
            for name in CLIENT_MAP.keys()
        }
        
        self._create_widgets()
        # self._setup_context_menu() # REMOVED: No manual IP setting in broadcast mode
        self._start_gui_update_loop()
        
        self.toggle_server() 

    def _reset_counters(self):
        global PACKETS_RECEIVED, PACKETS_ROUTED
        PACKETS_RECEIVED = 0
        PACKETS_ROUTED = 0
        self.received_label.config(text="Received: 0")
        self.routed_label.config(text="Routed: 0")

    def _create_widgets(self):
        # Row 0: Header and Controls
        header_frame = tk.LabelFrame(self, text="  Control Panel  ", bg=self.BG_DARK, fg=self.ACCENT_GREEN, 
                                      font=('Inter', 11, 'bold'), relief=tk.RIDGE, bd=3,
                                      labelanchor='n', padx=15, pady=15)
        header_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=5)
        
        # Title Section
        title_label = ttk.Label(header_frame, text="SharkeeHaptics Broadcast Router", style='Header.TLabel')
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
        
        # Row 0.5: Last Received OSC Message Display
        last_msg_frame = tk.LabelFrame(self, text="  Last Received VRChat Message  ", bg=self.BG_DARK, fg=self.ACCENT_CYAN,
                                        font=('Inter', 10, 'bold'), relief=tk.RIDGE, bd=3,
                                        labelanchor='n', padx=10, pady=8)
        last_msg_frame.grid(row=1, column=0, sticky="ew", padx=10, pady=5)
        
        self.last_msg_label = ttk.Label(last_msg_frame, text="Address: None | Value: 0.00", foreground='#FBBF24', font=('Inter', 10, 'bold'))
        self.last_msg_label.pack(anchor='w', padx=5)
        
        # Row 0.75: Action Buttons
        buttons_frame = tk.LabelFrame(self, text=" Actions ", bg=self.BG_DARK, fg=self.ACCENT_GREEN,
                                      font=('Inter', 10, 'bold'), relief=tk.RIDGE, bd=3,
                                      labelanchor='n', padx=10, pady=10)
        buttons_frame.grid(row=2, column=0, sticky="ew", padx=10, pady=5)
        
        ttk.Button(buttons_frame, text="Test All Clients", command=self._test_all_clients, style='TButton').pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_frame, text="Clear Log", command=self._clear_log, style='TButton').pack(side=tk.LEFT, padx=5)
        ttk.Button(buttons_frame, text="Help/About", command=self._show_help_about, style='TButton').pack(side=tk.LEFT, padx=5)
        
        # Row 3: Status Table - SIMPLIFIED COLUMNS
        status_frame = tk.LabelFrame(self, text=f" Client Broadcast Status (Target IP: {BROADCAST_IP}:{INTERNAL_OSC_PORT}) ", 
                                     bg=self.BG_DARK, fg=self.ACCENT_CYAN,
                                     font=('Inter', 11, 'bold'), relief=tk.RIDGE, bd=3,
                                     labelanchor='n', padx=10, pady=10)
        status_frame.grid(row=3, column=0, sticky="nsew", padx=10, pady=5)
        status_frame.grid_columnconfigure(0, weight=1)
        status_frame.grid_rowconfigure(0, weight=1)

        # Removed 'resolved_ip' column
        columns = ("vrc_receiver", "mdns_hostname", "intensity", "status")
        self.tree = ttk.Treeview(status_frame, columns=columns, show="headings", height=12)
        
        self.tree.heading("vrc_receiver", text="VRC Receiver", anchor=tk.W)
        self.tree.heading("mdns_hostname", text="Role (mDNS Hostname)", anchor=tk.W)
        self.tree.heading("intensity", text="Intensity", anchor=tk.CENTER)
        self.tree.heading("status", text="Status", anchor=tk.CENTER)

        self.tree.column("vrc_receiver", width=250, anchor=tk.W)
        self.tree.column("mdns_hostname", width=250, anchor=tk.W)
        self.tree.column("intensity", width=100, anchor=tk.CENTER)
        self.tree.column("status", width=100, anchor=tk.CENTER)
        
        self.tree.grid(row=0, column=0, sticky="nsew")

        # Initialize table data
        for name, hostname in CLIENT_MAP.items():
            self.tree.insert('', 'end', iid=name, values=(name, hostname, '0.00%', 'STOPPED'))

        # Row 4: Activity Log
        log_frame = tk.LabelFrame(self, text=" Activity Log ", bg=self.BG_DARK, fg=self.ACCENT_GREEN, 
                                  font=('Inter', 10, 'bold'), relief=tk.RIDGE, bd=3,
                                  labelanchor='n', padx=10, pady=10)
        log_frame.grid(row=4, column=0, sticky="nsew", padx=10, pady=5)
        log_frame.grid_columnconfigure(0, weight=1)
        log_frame.grid_rowconfigure(0, weight=1)
        
        self.log_text = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, height=8, bg=self.BG_LIGHT, fg='#E5E7EB', font=('Inter', 9), borderwidth=0, relief=tk.FLAT)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        
        # Configure tags for log coloring
        for level, color in self.LOG_COLORS.items():
            self.log_text.tag_config(level, foreground=color)
            
        self.log_to_gui("GUI initialized.", level='INFO')

    # Removed _setup_context_menu and IP management functions

    def _send_osc_via_broadcast(self, address, value):
        """Helper to send OSC via the global broadcast function."""
        try:
            send_osc_via_broadcast(address, value)
            return True
        except Exception as e:
            self.log_to_gui(f"Error sending broadcast OSC message to {address}: {e}", level='ERROR')
            return False

    def _test_single_client(self):
        """Sends a strong test pulse for the selected client role via broadcast."""
        selected_iid = self.tree.selection()[0] if self.tree.selection() else None
        if not selected_iid:
            return
            
        client_receiver_name = selected_iid.lower()
        full_osc_address = f"{INTERNAL_OSC_ADDRESS_BASE}/{client_receiver_name}"
        
        self.log_to_gui(f"Sending test pulse for role {selected_iid} via broadcast.", level='WARN')
        if self._send_osc_via_broadcast(full_osc_address, 1.0): # Send max intensity pulse
            # Schedule a stop pulse after a short delay
            self.after(200, lambda: self._send_osc_via_broadcast(full_osc_address, 0.0))
            
    def _test_all_clients(self):
        """Sends a strong test pulse to all client roles via broadcast."""
        self.log_to_gui("Sending test pulse to ALL client roles via broadcast.", level='WARN')
        
        sent_count = 0
        for receiver in CLIENT_MAP.keys():
            client_receiver_name = receiver.lower()
            full_osc_address = f"{INTERNAL_OSC_ADDRESS_BASE}/{client_receiver_name}"
            if self._send_osc_via_broadcast(full_osc_address, 1.0):
                sent_count += 1
        
        if sent_count > 0:
            # Schedule a stop pulse after a short delay
            self.after(200, self._stop_all_clients)
            self.log_to_gui(f"Broadcast a pulse message for {sent_count} client roles.", level='SUCCESS')
        else:
            self.log_to_gui("Error: Could not broadcast test pulse.", level='ERROR')

    def _stop_all_clients(self):
        """Sends a stop command to all client roles."""
        for receiver in CLIENT_MAP.keys():
            client_receiver_name = receiver.lower()
            full_osc_address = f"{INTERNAL_OSC_ADDRESS_BASE}/{client_receiver_name}"
            # Use the helper but suppress GUI logging for quiet stop
            try:
                send_osc_via_broadcast(full_osc_address, 0.0) 
            except:
                pass # Ignore errors on stop, best effort

    def _refresh_all_clients(self):
        """Resets status to reflect that broadcasting is active."""
        for name in CLIENT_MAP.keys():
            self.client_status[name]['status'] = 'BROADCASTING'
        self._update_client_table_all()
        self.log_to_gui("Client list refreshed. Status reflects broadcast target.", level='INFO')


    def log_to_gui(self, message, level='INFO'):
        """Thread-safe logging function for the GUI text area."""
        timestamp = time.strftime("[%H:%M:%S]")
        self.log_text.config(state=tk.NORMAL) 
        self.log_text.insert(tk.END, f"{timestamp} ", 'timestamp')
        self.log_text.insert(tk.END, f"[{level}] ", level)
        self.log_text.insert(tk.END, f"{message}\n", 'message')
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)


    def toggle_server(self):
        """Starts or stops the OSC server."""
        if self.is_running:
            self._stop_server()
        else:
            self._start_server()

    def _start_server(self):
        """Initializes and starts the OSC server thread."""
        try:
            d = dispatcher.Dispatcher()
            # Map all VRChat receiver addresses to the single handler
            for address in VRC_OSC_MAP.keys():
                d.map(address, sharkeehaptics_router_handler)
                
            self.server = osc_server.ThreadingOSCUDPServer(
                (VRC_OSC_LISTEN_IP, VRC_OSC_LISTEN_PORT), d)
            
            self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
            self.server_thread.start()

            # No resolver thread to start
            
            self.is_running = True
            self.status_label.config(text=f"STATUS: RUNNING (VRC Port: {VRC_OSC_LISTEN_PORT})", foreground='#4CAF50')
            self.toggle_button.config(text="Stop Router")
            self.log_to_gui(f"OSC Broadcast Router started. Listening for VRChat on {VRC_OSC_LISTEN_IP}:{VRC_OSC_LISTEN_PORT}. Broadcasting to {BROADCAST_IP}:{INTERNAL_OSC_PORT}.", level='SUCCESS')
            self._refresh_all_clients() # Kick off initial status display
        except Exception as e:
            self.log_to_gui(f"Failed to start server: {e}", level='ERROR')
            self.status_label.config(text="STATUS: FAILED", foreground='#FF5722')
            messagebox.showerror("Startup Error", f"Could not start the OSC server.\n\nError: {e}\n\nIs VRChat or another application already using port {VRC_OSC_LISTEN_PORT}?")

    def _stop_server(self):
        """Stops the OSC server gracefully."""
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server_thread.join(timeout=1)
        
        self.is_running = False
        self.status_label.config(text="STATUS: STOPPED", foreground='#FF5722')
        self.toggle_button.config(text="Start Router")
        self.log_to_gui("OSC Router stopped.", level='INFO')
        self._reset_counters()
        
        # Clear all connection statuses in the table
        for name in CLIENT_MAP.keys():
            self._update_client_status({'receiver': name, 'ip': BROADCAST_IP, 'intensity': 0.0, 'status': 'STOPPED'})

    def _start_gui_update_loop(self):
        """Sets up the periodic GUI update loop."""
        self.after(GUI_UPDATE_INTERVAL_MS, self._process_queue)
        
    def _process_queue(self):
        """Processes messages from the background threads to update the GUI."""
        
        items_processed = 0
        while not GUI_QUEUE.empty() and items_processed < MAX_GUI_ITEMS_PER_LOOP:
            try:
                message = GUI_QUEUE.get_nowait()
                msg_type = message.get('type')
                
                if msg_type == 'LOG':
                    self.log_to_gui(message['message'], message['level'])
                    
                elif msg_type == 'STATUS_UPDATE':
                    self._update_client_status(message)

                # Removed RESOLVE_UPDATE case
                        
                elif msg_type == 'LAST_MESSAGE_UPDATE':
                    self.last_msg_label.config(text=f"Address: {message['address']} | Value: {message['value']:.2f}")

                elif msg_type == 'COUNTER_UPDATE':
                    self.received_label.config(text=f"Received: {message['received']}")
                    self.routed_label.config(text=f"Routed: {message['routed']}")
                    
                GUI_QUEUE.task_done()
                items_processed += 1
            except queue.Empty:
                break
            except Exception as e:
                self.log_to_gui(f"Error processing GUI queue message: {e}", level='ERROR')
                GUI_QUEUE.task_done()
                
        self.after(GUI_UPDATE_INTERVAL_MS, self._process_queue)

    def _update_client_status(self, data):
        """Updates a single client row in the Treeview table."""
        receiver = data['receiver']
        
        # 1. Update the local cache
        self.client_status[receiver]['ip'] = BROADCAST_IP
        self.client_status[receiver]['status'] = data['status']
        if data.get('intensity') is not None:
             self.client_status[receiver]['intensity'] = data['intensity']

        # 2. Update the Treeview
        if receiver in self.tree.get_children():
            item_id = receiver
            
            # Format intensity for display
            intensity_val = self.client_status[receiver]['intensity']
            intensity_str = f"{intensity_val * 100:.0f}%" if intensity_val > 0.01 else "0.00%"
            
            # Update the item with the simplified column set
            self.tree.item(item_id, values=(
                receiver, 
                CLIENT_MAP.get(receiver), 
                intensity_str, 
                self.client_status[receiver]['status']
            ))
            
            # Update row appearance based on status/activity
            new_tags = ['base']
            self.tree.tag_configure('base', foreground='#E5E7EB', background=self.BG_LIGHT, font=('Inter', 10, 'normal'))
            
            if data['status'] == 'STOPPED': new_tags.append('offline'); self.tree.tag_configure('offline', foreground='#FF5722')
            else: new_tags.append('broadcasting'); self.tree.tag_configure('broadcasting', foreground=self.ACCENT_CYAN)
            
            is_active = self.client_status[receiver]['intensity'] > 0.05
            if is_active:
                new_tags.append('active')
                self.tree.tag_configure('active', background='#4B5563', foreground='#FBBF24', font=('Inter', 10, 'bold'))
            
            self.tree.item(item_id, tags=new_tags)
    
    def _update_client_table_all(self):
        """Forces a refresh of all client rows."""
        for receiver, status_data in self.client_status.items():
            self._update_client_status({
                'receiver': receiver, 'ip': status_data['ip'], 
                'intensity': status_data['intensity'], 'status': status_data['status']
            })
            
    def on_closing(self):
        """Handles graceful shutdown when the window is closed."""
        self.log_to_gui("Application closing...", level='INFO')
        self._stop_server() # Ensure threads are stopped
        self.destroy()

    def _clear_log(self):
        """Clears the activity log window."""
        self.log_text.config(state=tk.NORMAL)
        self.log_text.delete('1.0', tk.END)
        self.log_text.config(state=tk.DISABLED)
        self.log_to_gui("Activity log cleared.", level='INFO')
        
    def _show_help_about(self):
        """Displays help information."""
        messagebox.showinfo(
            "SharkeeHaptics Router Help (BROADCAST MODE)",
            "This router forwards haptic intensity OSC messages from VRChat to ALL ESP8266 clients using a single UDP broadcast message (255.255.255.255).\n\n"
            "Each client unit processes the message based on the **Role (mDNS Hostname)** embedded in the OSC address.\n\n"
            "1. **VRC Receiver**: The parameter name in your VRChat avatar.\n"
            "2. **Role (mDNS Hostname)**: The name the ESP8266 client is configured as (e.g., 'chest.local').\n"
            "3. **Status**: BROADCASTING (Router is sending data for this role) or STOPPED."
        )

if __name__ == "__main__":
    app = SharkeeHapticsRouterApp()
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()