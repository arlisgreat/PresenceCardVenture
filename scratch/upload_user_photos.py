import os, uuid, urllib.request, urllib.parse, json
os.chdir(os.path.dirname(os.path.abspath(__file__)))

BASE = "http://172.20.10.7:3000/v1"
UPLOADS = [
    ("shrine.jpg", "demo-user-2", "今天的我 bruh"),          # 涂鸦自拍 by 墨墨
    ("doodle.jpg", "demo-user-3", "冲绳的太阳好晒"),          # 神社旅行 by 露娜
    ("pixel.jpg",  "demo-user-2", "决赛夜，我们仨还在赶工"),   # hackathon by 墨墨
]

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
for fname, token, caption in UPLOADS:
    with open(fname, "rb") as f:
        body = f.read()
    req = urllib.request.Request(BASE + "/photos", data=body, method="POST")
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Content-Type", "image/jpeg")
    req.add_header("Idempotency-Key", str(uuid.uuid4()))
    req.add_header("X-Caption", urllib.parse.quote(caption))
    with opener.open(req, timeout=15) as resp:
        print(fname, resp.status, resp.read().decode()[:120])
