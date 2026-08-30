import serial, sys, time, datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
LOG = sys.argv[3] if len(sys.argv) > 3 else None

KEY = ("error", "err", "fail", "panic", "abort", "assert", "boot", "wifi",
       "pair", "prov", "http", "upload", "feed", "rst", "guru", "backtrace",
       "e (", "w (")

def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

logf = open(LOG, "a", encoding="utf-8", errors="replace") if LOG else None
last_err = None

while True:
    try:
        s = serial.Serial()
        s.port = PORT
        s.baudrate = BAUD
        s.timeout = 1
        s.dtr = False
        s.rts = False
        s.open()
        print(f"[serial] connected {PORT} @ {BAUD}", flush=True)
        buf = b""
        while True:
            chunk = s.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if logf:
                    logf.write(f"{stamp()} {text}\n")
                    logf.flush()
                low = text.lower()
                if "get /feed" in low and " 200 " in text:
                    continue  # 常规轮询成功，不上报
                if any(k in low for k in KEY):
                    print(f"{stamp()} {text}", flush=True)
    except serial.SerialException as e:
        msg = f"[serial] waiting for {PORT} ({e.__class__.__name__})"
        if msg != last_err:
            print(msg, flush=True)
            last_err = msg
        time.sleep(3)
    except KeyboardInterrupt:
        break
