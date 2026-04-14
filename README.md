# NexLink — ESP32 Networked I/O Controller — v2.0.0
### ESP32 Networked I/O Controller

---

## What's New vs v1.0

| Feature | v1.0 (Original) | v2.0 (Pro) |
|---|---|---|
| Protocol | Raw text (`LED_ON\n`) | JSON (`{"cmd":"LED_ON","token":"..."}`) |
| Auth | None | Token-based auth on every command |
| Clients | 1 at a time | Up to 4 simultaneous (FreeRTOS task per client) |
| GPIO | GPIO 2 only | GPIO 2, 4, 5 (configurable array) |
| PWM | None | LEDC PWM on GPIO 18, 19 (0–100%) |
| Telemetry | None | Heap, uptime, client count, GPIO states |
| Heartbeat | None | 10-second idle timeout, auto-close |
| Python client | Simple loop | TUI dashboard, reconnect, history, RTT |

---

## Hardware

- ESP32 development board (any variant)
- USB cable
- Optional: LED + 330Ω resistor on GPIO 2, 4, 5
- Optional: PWM-controlled device (servo/motor driver) on GPIO 18, 19
- PC and ESP32 on the **same Wi-Fi network**

---

## Project Structure

```
esp32_tcp_pro/
├── main/
│   ├── tcp_server_pro.c     ← ESP32 firmware
│   └── CMakeLists.txt
├── tcp_client_pro.py        ← Python TUI client
└── README.md
```

---

## Build & Flash

```bash
# 1. Create project
idf.py create-project esp32_tcp_pro
cd esp32_tcp_pro

# 2. Replace source files (copy from this repo)

# 3. Set your Wi-Fi credentials in tcp_server_pro.c
#    #define WIFI_SSID  "your_ssid"
#    #define WIFI_PASS  "your_password"

# 4. (Optional) Change the auth token
#    #define AUTH_TOKEN "NEXLINK2024"

# 5. Build, flash, and monitor
idf.py build flash monitor
```

---

## Configuration (in `tcp_server_pro.c`)

```c
#define WIFI_SSID     "your_ssid"
#define WIFI_PASS     "your_password"
#define TCP_PORT      3333
#define MAX_CLIENTS   4             // concurrent connections
#define AUTH_TOKEN    "NEXLINK2024"
#define HEARTBEAT_MS  10000         // idle timeout in ms

// GPIO pins controlled by LED_ON/LED_OFF
static const gpio_num_t GPIO_PINS[] = { GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5 };

// PWM pins controlled by PWM command
static const gpio_num_t PWM_PINS[]  = { GPIO_NUM_18, GPIO_NUM_19 };
```

---

## JSON Protocol

All messages are **newline-terminated JSON** (`\n`).

### Requests (Client → ESP32)

| Command | JSON |
|---|---|
| Ping | `{"cmd":"PING","token":"NEXLINK2024"}` |
| LED on (default pin 2) | `{"cmd":"LED_ON","token":"NEXLINK2024"}` |
| LED on (specific pin) | `{"cmd":"LED_ON","token":"NEXLINK2024","pin":4}` |
| LED off | `{"cmd":"LED_OFF","token":"NEXLINK2024","pin":2}` |
| All off | `{"cmd":"ALL_OFF","token":"NEXLINK2024"}` |
| PWM 75% on pin 18 | `{"cmd":"PWM","token":"NEXLINK2024","pin":18,"duty":75}` |
| Status | `{"cmd":"STATUS","token":"NEXLINK2024"}` |
| Telemetry | `{"cmd":"TELEMETRY","token":"NEXLINK2024"}` |
| Reboot | `{"cmd":"REBOOT","token":"NEXLINK2024"}` |

### Responses (ESP32 → Client)

**Success:**
```json
{"status":"ok","cmd":"LED_ON","pin":2,"state":1}
```

**Telemetry response:**
```json
{
  "status":"ok",
  "cmd":"TELEMETRY",
  "uptime_s":342,
  "free_heap":210432,
  "clients":2,
  "gpio":{"2":1,"4":0,"5":0},
  "pwm":{"18":75,"19":0}
}
```

**Error:**
```json
{"status":"err","code":"AUTH_FAIL","msg":"invalid or missing token"}
```

### Error codes

| Code | Cause |
|---|---|
| `AUTH_FAIL` | Token missing or wrong |
| `BAD_CMD` | `cmd` field missing |
| `UNKNOWN_CMD` | Command not recognised |
| `BAD_PIN` | Pin not in allowed list |
| `SERVER_FULL` | All 4 client slots occupied |

---

## Python Client Usage

```bash
# Default (uses IP in script)
python tcp_client_pro.py

# Override IP/port
python tcp_client_pro.py --ip 192.168.1.105 --port 3333
```

### Interactive commands

```
esp32> led on           # GPIO 2 on
esp32> led on 4         # GPIO 4 on
esp32> led off 4        # GPIO 4 off
esp32> all off          # all GPIO + PWM off
esp32> pwm 18 75        # 75% duty on GPIO 18
esp32> status           # GPIO state snapshot
esp32> telemetry        # full system info
esp32> ping             # round-trip latency
esp32> reboot           # soft-reboot ESP32
esp32> reconnect        # force reconnect
esp32> telem            # show cached telemetry
esp32> help             # command list
esp32> quit             # exit
```

Raw JSON is also accepted:
```
esp32> {"cmd":"LED_ON","token":"NEXLINK2024","pin":5}
```

---

## Security Notes

- The `AUTH_TOKEN` is sent in **plaintext** — fine for local LAN use.
- For production/Internet-facing use, switch to TLS (esp-tls) and
  use HMAC-SHA256 challenge-response instead of static token.
- The `REBOOT` command is intentionally gated behind a Python `y/N`
  confirmation prompt.

---

## Extending the Protocol

Adding a new command takes **3 steps**:

1. Add a `strcmp(cmd, "MY_CMD")` branch in `process_command()`.
2. Parse any extra JSON fields with `json_get_int()` / `json_get_str()`.
3. Add the corresponding `parse_input()` branch in the Python client.

The JSON framing means you can add optional fields without breaking
existing clients — they simply ignore unknown keys.
