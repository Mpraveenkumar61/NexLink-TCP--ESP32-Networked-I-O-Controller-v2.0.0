#!/usr/bin/env python3
"""
================================================================
  NexLink — ESP32 I/O Client — TUI Dashboard
  Version 2.0.0

  
  Company   : praveenkumar individual project
  

  Features:
  ─────────────────────────────────────────────────────────
  • JSON protocol with auth token
  • Auto-reconnect with exponential back-off
  • Live telemetry panel (refreshes every 3s)
  • Command history (↑/↓ arrows)
  • Multi-pin GPIO & PWM control
  • Colour-coded responses
  • Keyboard shortcuts
  ─────────────────────────────────────────────────────────
  Usage:
    python tcp_client_pro.py
    python tcp_client_pro.py --ip 192.168.1.105 --port 3333
================================================================
"""

import socket
import json
import time
import threading
import argparse
import sys
import os
import readline   # command history (Linux/macOS); graceful on Windows

# ── Configuration ─────────────────────────────────────────
DEFAULT_IP    = "192.168.31.154"   # Change to your ESP32 IP
DEFAULT_PORT  = 3333
AUTH_TOKEN    = "NEXLINK2024"
RECV_TIMEOUT  = 5.0               # seconds
HEARTBEAT_INT = 8.0               # send PING every N seconds
TELEMETRY_INT = 5.0               # auto-fetch telemetry every N seconds
MAX_RETRIES   = 5
RETRY_BASE    = 1.0               # exponential back-off base (seconds)
HISTORY_FILE  = os.path.expanduser("~/.esp32_client_history")

# ── ANSI colour helpers ────────────────────────────────────
class C:
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    GREEN   = "\033[92m"
    RED     = "\033[91m"
    YELLOW  = "\033[93m"
    CYAN    = "\033[96m"
    BLUE    = "\033[94m"
    MAGENTA = "\033[95m"
    WHITE   = "\033[97m"

    @staticmethod
    def ok(s):     return f"{C.GREEN}{s}{C.RESET}"
    @staticmethod
    def err(s):    return f"{C.RED}{s}{C.RESET}"
    @staticmethod
    def warn(s):   return f"{C.YELLOW}{s}{C.RESET}"
    @staticmethod
    def info(s):   return f"{C.CYAN}{s}{C.RESET}"
    @staticmethod
    def label(s):  return f"{C.BOLD}{C.WHITE}{s}{C.RESET}"
    @staticmethod
    def dim(s):    return f"{C.DIM}{s}{C.RESET}"

# ── Pretty-print JSON response ─────────────────────────────
def pretty_response(raw: str) -> str:
    try:
        obj = json.loads(raw.strip())
    except json.JSONDecodeError:
        return C.warn(f"[non-JSON] {raw.strip()}")

    status = obj.get("status", "?")
    cmd    = obj.get("cmd", "")
    prefix = C.ok("✔") if status == "ok" else C.err("✘")

    lines = [f"{prefix} {C.label(cmd or status)}"]

    for k, v in obj.items():
        if k in ("status", "cmd"):
            continue
        if isinstance(v, dict):
            lines.append(f"   {C.dim(k)}:")
            for dk, dv in v.items():
                state_str = ""
                if isinstance(dv, int) and k == "gpio":
                    state_str = (C.ok(" ON") if dv else C.dim(" OFF"))
                lines.append(f"      {C.info(dk)}: {dv}{state_str}")
        else:
            lines.append(f"   {C.dim(k)}: {C.info(str(v))}")

    return "\n".join(lines)

# ── Build JSON command ─────────────────────────────────────
def build_cmd(cmd: str, **kwargs) -> str:
    payload = {"cmd": cmd, "token": AUTH_TOKEN}
    payload.update(kwargs)
    return json.dumps(payload) + "\n"

# ── ESP32Client ────────────────────────────────────────────
class ESP32Client:
    def __init__(self, ip: str, port: int):
        self.ip          = ip
        self.port        = port
        self.sock        = None
        self.connected   = False
        self._lock       = threading.Lock()
        self._stop_event = threading.Event()
        self._telemetry  = {}
        self._hb_thread  = None
        self._tel_thread = None

    # ── Connection management ──────────────────────────────
    def connect(self, retries: int = MAX_RETRIES) -> bool:
        for attempt in range(retries):
            try:
                delay = RETRY_BASE * (2 ** attempt)
                if attempt > 0:
                    print(C.warn(f"  Retry {attempt}/{retries-1} in {delay:.1f}s..."))
                    time.sleep(delay)

                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(RECV_TIMEOUT)
                s.connect((self.ip, self.port))

                with self._lock:
                    self.sock      = s
                    self.connected = True

                banner_raw = s.recv(512).decode("utf-8", errors="replace")
                banner = json.loads(banner_raw.strip())
                print(C.ok(f"  Connected!  {banner.get('msg','')}"))

                self._stop_event.clear()
                self._start_background_threads()
                return True

            except (socket.timeout, ConnectionRefusedError, OSError) as e:
                print(C.err(f"  Connection attempt {attempt+1} failed: {e}"))

        return False

    def disconnect(self):
        self._stop_event.set()
        with self._lock:
            self.connected = False
            if self.sock:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
        if self._hb_thread:
            self._hb_thread.join(timeout=2)
        if self._tel_thread:
            self._tel_thread.join(timeout=2)
        print(C.dim("  Disconnected."))

    def reconnect(self) -> bool:
        print(C.warn("\n  Connection lost — attempting reconnect..."))
        self.disconnect()
        return self.connect()

    # ── Send / receive ─────────────────────────────────────
    def send_cmd(self, raw_json: str) -> str | None:
        with self._lock:
            if not self.connected or not self.sock:
                return None
            try:
                self.sock.sendall(raw_json.encode("utf-8"))
                resp = self.sock.recv(1024).decode("utf-8", errors="replace")
                return resp.strip()
            except (socket.timeout, OSError) as e:
                self.connected = False
                print(C.err(f"  Send error: {e}"))
                return None

    def send_and_print(self, raw_json: str):
        resp = self.send_cmd(raw_json)
        if resp is None:
            print(C.err("  No response — connection may be lost."))
            if not self.connected:
                if not self.reconnect():
                    print(C.err("  Could not reconnect. Type 'quit' to exit."))
        else:
            print(pretty_response(resp))
        return resp

    # ── Background threads ─────────────────────────────────
    def _start_background_threads(self):
        self._hb_thread = threading.Thread(
            target=self._heartbeat_loop, daemon=True)
        self._hb_thread.start()

        self._tel_thread = threading.Thread(
            target=self._telemetry_loop, daemon=True)
        self._tel_thread.start()

    def _heartbeat_loop(self):
        while not self._stop_event.wait(HEARTBEAT_INT):
            if not self.connected:
                break
            resp = self.send_cmd(build_cmd("PING"))
            if resp is None:
                break   # reconnect handled by main thread on next command

    def _telemetry_loop(self):
        while not self._stop_event.wait(TELEMETRY_INT):
            if not self.connected:
                break
            resp = self.send_cmd(build_cmd("TELEMETRY"))
            if resp:
                try:
                    self._telemetry = json.loads(resp)
                except json.JSONDecodeError:
                    pass

    def get_telemetry(self) -> dict:
        return self._telemetry.copy()

# ── CLI help ───────────────────────────────────────────────
HELP = f"""
{C.label("Available commands")}
─────────────────────────────────────────────────────
  {C.info("led on  [pin]")}         Turn GPIO pin ON    (default pin 2)
  {C.info("led off [pin]")}         Turn GPIO pin OFF   (default pin 2)
  {C.info("all off")}               Turn ALL outputs OFF
  {C.info("pwm [pin] [0-100]")}     Set PWM duty cycle  (default pin 18)
  {C.info("status")}                Get all GPIO states
  {C.info("telemetry")}             Full system telemetry
  {C.info("ping")}                  Round-trip latency check
  {C.info("reboot")}                Soft-reboot the ESP32
  {C.info("reconnect")}             Force reconnect
  {C.info("telem")}                 Show last cached telemetry
  {C.info("help")}                  Show this message
  {C.info("quit / exit / q")}       Disconnect and exit
─────────────────────────────────────────────────────
{C.dim("Raw JSON also accepted, e.g.:")}
  {C.dim('{"cmd":"LED_ON","token":"NEXLINK2024","pin":4}')}
"""

# ── Parse interactive input → JSON command ─────────────────
def parse_input(raw: str) -> str | None:
    """
    Returns a JSON string to send, or None if unrecognised.
    Raises SystemExit for quit commands.
    """
    s = raw.strip().lower()
    parts = raw.strip().split()

    if s in ("quit", "exit", "q"):
        raise SystemExit(0)

    if s == "help":
        print(HELP)
        return None

    if s == "ping":
        return build_cmd("PING")

    if s in ("status",):
        return build_cmd("STATUS")

    if s == "telemetry":
        return build_cmd("TELEMETRY")

    if s == "all off":
        return build_cmd("ALL_OFF")

    if s == "reboot":
        confirm = input(C.warn("  Reboot ESP32? [y/N]: ")).strip().lower()
        if confirm != "y":
            print(C.dim("  Cancelled."))
            return None
        return build_cmd("REBOOT")

    if s.startswith("led on"):
        pin = int(parts[2]) if len(parts) >= 3 else 2
        return build_cmd("LED_ON", pin=pin)

    if s.startswith("led off"):
        pin = int(parts[2]) if len(parts) >= 3 else 2
        return build_cmd("LED_OFF", pin=pin)

    if s.startswith("pwm"):
        pin  = int(parts[1]) if len(parts) >= 2 else 18
        duty = int(parts[2]) if len(parts) >= 3 else 50
        return build_cmd("PWM", pin=pin, duty=duty)

    # Allow raw JSON pass-through
    if raw.strip().startswith("{"):
        return raw.strip() + "\n"

    return None  # unrecognised

# ── Status bar ─────────────────────────────────────────────
def print_status_bar(client: ESP32Client):
    tel = client.get_telemetry()
    if not tel:
        return
    heap    = tel.get("free_heap", "?")
    uptime  = tel.get("uptime_s",  "?")
    clients = tel.get("clients",   "?")
    gpio    = tel.get("gpio",      {})
    gpio_str = "  ".join(
        f"GPIO{k}:{C.ok('ON') if v else C.dim('off')}"
        for k, v in gpio.items()
    )
    print(
        C.dim("─" * 55) + "\n" +
        C.dim(f"  Heap: {heap}B  |  Uptime: {uptime}s  "
              f"|  Clients: {clients}  |  {gpio_str}")
    )

# ── Main ───────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="NexLink TCP Client")
    parser.add_argument("--ip",   default=DEFAULT_IP,   help="ESP32 IP")
    parser.add_argument("--port", default=DEFAULT_PORT,
                        type=int, help="TCP port")
    args = parser.parse_args()

    # readline history
    try:
        readline.set_history_length(200)
        if os.path.exists(HISTORY_FILE):
            readline.read_history_file(HISTORY_FILE)
    except Exception:
        pass

    print(f"""
{C.BOLD}╔══════════════════════════════════════════╗
║   NexLink — ESP32 I/O Client          ║
║   v2.0.0  —  NexLink Automation        ║
╚══════════════════════════════════════════╝{C.RESET}
  Target : {C.info(args.ip)}:{C.info(str(args.port))}
  Token  : {C.dim(AUTH_TOKEN)}
""")

    client = ESP32Client(args.ip, args.port)

    print(C.label("  Connecting..."))
    if not client.connect():
        print(C.err("  Failed to connect after retries. Exiting."))
        sys.exit(1)

    print(HELP)
    print_status_bar(client)

    try:
        while True:
            try:
                raw = input(f"\n{C.BOLD}{C.CYAN}esp32>{C.RESET} ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not raw:
                continue

            if raw.lower() == "reconnect":
                client.reconnect()
                continue

            if raw.lower() == "telem":
                tel = client.get_telemetry()
                if tel:
                    print(pretty_response(json.dumps(tel)))
                else:
                    print(C.dim("  No telemetry yet — waiting for background fetch"))
                continue

            try:
                cmd_json = parse_input(raw)
            except SystemExit:
                break
            except ValueError as e:
                print(C.err(f"  Input error: {e}"))
                continue

            if cmd_json is None:
                if raw.lower() != "help":
                    print(C.warn(f"  Unknown command: '{raw}'  (type 'help')"))
                continue

            t0 = time.perf_counter()
            client.send_and_print(cmd_json)
            elapsed_ms = (time.perf_counter() - t0) * 1000
            print(C.dim(f"  RTT: {elapsed_ms:.1f} ms"))

            # Show status bar every few commands
            print_status_bar(client)

    finally:
        try:
            readline.write_history_file(HISTORY_FILE)
        except Exception:
            pass
        client.disconnect()
        print(C.dim("\n  Goodbye."))

if __name__ == "__main__":
    main()