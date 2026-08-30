import json, urllib.request, urllib.error

BASE = "http://127.0.0.1:3000/v1"

def req(method, path, body=None, token=None):
    url = BASE + path
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Content-Type", "application/json")
    if token:
        r.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(r, timeout=5) as resp:
            return resp.status, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")

st, u = req("POST", "/auth/register", {"username": "e2e_cam", "password": "pw123456", "display_name": "E2E"})
if st >= 400:
    st, u = req("POST", "/auth/login", {"username": "e2e_cam", "password": "pw123456"})
print("auth:", st, list(u) if isinstance(u, dict) else u)
sess = u.get("token") or (u.get("session") or {}).get("token")
assert sess, u

st, pc = req("POST", "/pair/code", {"device_id": "dvc_e2e", "fw_version": "0.2.0"})
print("pair/code:", st, pc)
st, b = req("POST", "/pair/bind", {"device_id": "dvc_e2e", "pair_code": pc["pair_code"]}, token=sess)
print("pair/bind:", st, b.get("status"))
st, ps = req("GET", f"/pair/status?device_id=dvc_e2e&pair_code={pc['pair_code']}")
print("pair/status:", st, ps.get("status"))
dtok = ps["device_token"]

st, feed = req("GET", "/feed", token=dtok)
items = feed.get("items") or feed.get("photos") or []
print("feed:", st, "items:", len(items))
pid = None
if items:
    pid = items[0].get("photo_id") or items[0].get("id")

if not pid:
    # empty feed: upload a minimal JPEG as the device, then react to it
    jpeg = bytes([0xFF, 0xD8, 0xFF, 0xD9])
    r = urllib.request.Request(BASE + "/photos", data=jpeg, method="POST")
    r.add_header("Content-Type", "image/jpeg")
    r.add_header("Authorization", "Bearer " + dtok)
    r.add_header("Idempotency-Key", "e2e-upload-1")
    r.add_header("X-Device-Id", "dvc_e2e")
    try:
        with urllib.request.urlopen(r, timeout=5) as resp:
            up = json.loads(resp.read())
            print("upload:", resp.status, up)
    except urllib.error.HTTPError as e:
        print("upload FAILED:", e.code, e.read().decode()[:200])
        raise SystemExit(1)
    pid = up.get("photo_id") or up.get("id") or (up.get("photo") or {}).get("id")
assert pid, "no photo id"

st1, r1 = req("POST", f"/photos/{pid}/reactions", {"type": "heart"}, token=dtok)
print("DEVICE POST /photos/:id/reactions ->", st1, r1)

st2, r2 = req("POST", "/reactions", {"photo_id": pid, "tap_count": 3}, token=dtok)
print("DEVICE POST /reactions (compat) ->", st2, r2)

print("RESULT:", "PASS" if (st1 == 201 and st2 == 201) else "FAIL")
