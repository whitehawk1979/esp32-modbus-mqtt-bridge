#!/usr/bin/env python3
"""WiFi POST OTA upload — single-attempt Digest auth, clean flow"""
import hashlib, time, re, sys, os, socket

ESP_IP = "192.168.1.45"
PORT = 80
USER = "admin"
PASS = "admin"
CHUNK = 64
DELAY = 0.010  # ~6KB/s — WDT fix allows this

FW = sys.argv[1] if len(sys.argv) > 1 else "/config/.hermes/projects/modbus-mqtt-ha/.pio/build/esp32-s3-prod/firmware.bin"

def parse_www_auth(text):
    params = {}
    for item in re.findall(r'(\w+)="([^"]*)"', text):
        params[item[0]] = item[1]
    qop = re.search(r'qop=(\w+)', text)
    if qop and 'qop' not in params:
        params['qop'] = qop.group(1)
    return params

def build_digest(user, password, method, uri, params):
    nc = '00000001'
    cnonce = 'tor265wifi'
    ha1 = hashlib.md5(f'{user}:{params["realm"]}:{password}'.encode()).hexdigest()
    ha2 = hashlib.md5(f'{method}:{uri}'.encode()).hexdigest()
    qop = params.get('qop', 'auth')
    resp = hashlib.md5(f'{ha1}:{params["nonce"]}:{nc}:{cnonce}:{qop}:{ha2}'.encode()).hexdigest()
    hdr = (f'Digest username="{user}", realm="{params["realm"]}", '
           f'nonce="{params["nonce"]}", uri="{uri}", qop={qop}, '
           f'nc={nc}, cnonce="{cnonce}", response="{resp}"')
    if 'opaque' in params:
        hdr += f', opaque="{params["opaque"]}"'
    return hdr

def get_nonce_once():
    """Single nonce request — no retry, no 429 risk"""
    s = socket.socket()
    s.settimeout(10)
    s.connect((ESP_IP, PORT))
    s.sendall(f'POST /otaupload HTTP/1.1\r\nHost: {ESP_IP}\r\nContent-Length: 0\r\n\r\n'.encode())
    d = b''
    while b'\r\n\r\n' not in d:
        try:
            c = s.recv(4096)
            if not c: break
            d += c
        except: break
    s.close()
    www = d.decode(errors='replace')
    if '429' in www[:50]:
        return None
    m = re.search(r'WWW-Authenticate: (.+?)\r\n', www)
    if not m:
        return None
    return parse_www_auth(m.group(1))

with open(FW, 'rb') as f:
    fw = f.read()
print(f'FW: {len(fw)} bytes ({len(fw)//1024}KB)', flush=True)

# Single nonce request
print('Getting nonce (single attempt)...', flush=True)
dp = get_nonce_once()
if dp is None:
    print('429 or no challenge — aborting. Wait 60s and retry.', flush=True)
    sys.exit(1)
print(f'Nonce: {dp["nonce"][:16]}...', flush=True)

# Build multipart body immediately
boundary = '----TOR265WIFI'
body = (
    f'--{boundary}\r\n'
    f'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
    f'Content-Type: application/octet-stream\r\n\r\n'
).encode() + fw + f'\r\n--{boundary}--\r\n'.encode()

auth = build_digest(USER, PASS, 'POST', '/otaupload', dp)

# Send immediately — no delay (nonce is fresh)
print(f'Uploading {len(body)//1024}KB at ~{CHUNK/DELAY/1024:.0f}KB/s via WiFi POST...', flush=True)
s = socket.socket()
s.settimeout(900)
s.connect((ESP_IP, PORT))

hdr = (
    f'POST /otaupload HTTP/1.1\r\n'
    f'Host: {ESP_IP}\r\n'
    f'Authorization: {auth}\r\n'
    f'Content-Type: multipart/form-data; boundary={boundary}\r\n'
    f'Content-Length: {len(body)}\r\n\r\n'
)
s.sendall(hdr.encode())

sent = 0
t0 = time.time()
last_log = 0

for i in range(0, len(body), CHUNK):
    chunk = body[i:i + CHUNK]
    try:
        s.sendall(chunk)
    except (BrokenPipeError, OSError) as e:
        print(f'Connection lost at {sent//1024}KB: {e}', flush=True)
        sys.exit(1)
    sent += len(chunk)
    time.sleep(DELAY)
    if sent - last_log >= 65536:
        spd = sent / (time.time() - t0) / 1024
        pct = sent * 100 / len(body)
        print(f'  {sent//1024}/{len(body)//1024}KB ({pct:.0f}%) {spd:.1f}KB/s', flush=True)
        last_log = sent

elapsed = time.time() - t0
print(f'Sent ALL {sent}/{len(body)}B in {elapsed:.0f}s', flush=True)

# Read response (ESP32 may reboot without HTTP response)
print('Waiting for ESP32 response (15s)...', flush=True)
s.settimeout(15)
resp = b''
while True:
    try:
        c = s.recv(4096)
        if not c: break
        resp += c
    except: break
s.close()

rt = resp.decode(errors='replace')
status = rt.split('\r\n')[0] if rt else 'empty'
print(f'HTTP: {status}', flush=True)
if '200' in status:
    print('WiFi POST OTA SUCCESS!', flush=True)
elif '500' in status:
    print('OTA write failed!', flush=True)
elif not resp or 'empty' in status:
    print('No HTTP response — ESP32 likely rebooting after WDT-safe flash!', flush=True)
else:
    body_txt = rt.split('\r\n\r\n', 1)
    if len(body_txt) > 1:
        print(f'Body: {body_txt[1][:300]}', flush=True)