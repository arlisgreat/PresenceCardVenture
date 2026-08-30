import os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
import math, random
from PIL import Image, ImageDraw, ImageFilter

W, H = 800, 480

def grad(stops):
    img = Image.new("RGB", (W, H)); px = img.load()
    def lerp(a, b, t): return tuple(int(a[i]+(b[i]-a[i])*t) for i in range(3))
    for y in range(H):
        t = y/(H-1)
        for i in range(len(stops)-1):
            t0,c0 = stops[i]; t1,c1 = stops[i+1]
            if t0 <= t <= t1:
                c = lerp(c0,c1,(t-t0)/(t1-t0)); break
        for x in range(W): px[x,y] = c
    return img

def grain(img, amt=0.05):
    g = Image.effect_noise((W,H), 14).convert("L")
    return Image.blend(img, Image.merge("RGB",(g,g,g)), amt)

# 1. Starry night
random.seed(11)
img = grad([(0,(8,10,28)),(0.6,(16,20,52)),(1,(34,30,70))])
band = Image.new("RGBA",(W,H),(0,0,0,0)); bd = ImageDraw.Draw(band)
for i in range(900):
    t = i/900; x = int(t*W); y = int(120 + 140*math.sin(t*3.1) + random.gauss(0,42))
    if 0 <= y < H: bd.ellipse([x-1,y-1,x+1,y+1], fill=(210,205,240,random.randint(8,26)))
band = band.filter(ImageFilter.GaussianBlur(5)); img.paste(band,(0,0),band)
d = ImageDraw.Draw(img, "RGBA")
for _ in range(240):
    x,y = random.randint(0,W-1), random.randint(0,H-1)
    r = random.choice([1,1,1,2]); a = random.randint(120,255)
    d.ellipse([x-r,y-r,x+r,y+r], fill=(255,250,235,a))
mx,my = 640,96
for r in range(46,30,-2): d.ellipse([mx-r,my-r,mx+r,my+r], fill=(240,238,220,10))
d.ellipse([mx-28,my-28,mx+28,my+28], fill=(246,243,224,255))
d.ellipse([mx-38,my-34,mx+16,my+20], fill=(20,22,50,40))
sil = ImageDraw.Draw(img)
pts = [(0,430)]
for x in range(0,W+1,40): pts.append((x, 430 - int(60*abs(math.sin(x/170))) - random.randint(0,18)))
pts += [(W,480),(0,480)]
sil.polygon(pts, fill=(10,10,22))
grain(img,0.04).save("starry_night.jpg","JPEG",quality=82)

# 2. Misty mountains
random.seed(23)
img = grad([(0,(214,226,235)),(0.5,(196,214,222)),(1,(168,190,196))])
for base,(cr,cg,cb) in [(150,(150,175,185)),(230,(115,145,158)),(310,(80,110,126)),(390,(48,74,92))]:
    lay = Image.new("RGBA",(W,H),(0,0,0,0)); ld = ImageDraw.Draw(lay)
    pts=[(0,H)]
    ph = random.uniform(0,6.28)
    for x in range(0,W+1,16):
        y = base - int(46*math.sin(x/140+ph) + 22*math.sin(x/57+ph*2))
        pts.append((x,y))
    pts.append((W,H))
    ld.polygon(pts, fill=(cr,cg,cb,235))
    lay = lay.filter(ImageFilter.GaussianBlur(1)); img.paste(lay,(0,0),lay)
mist = Image.new("RGBA",(W,H),(0,0,0,0)); md = ImageDraw.Draw(mist)
for _ in range(14):
    mx0 = random.randint(-100,W); my0 = random.randint(180,420)
    md.ellipse([mx0,my0,mx0+random.randint(200,420),my0+random.randint(20,44)], fill=(235,242,244,random.randint(50,90)))
mist = mist.filter(ImageFilter.GaussianBlur(12)); img.paste(mist,(0,0),mist)
d = ImageDraw.Draw(img,"RGBA")
for bx,by,s in [(210,120,11),(255,140,8),(560,100,9)]:
    d.arc([bx-s,by-s//2,bx,by+s//2],180,320,fill=(60,74,88),width=2)
    d.arc([bx,by-s//2,bx+s,by+s//2],220,360,fill=(60,74,88),width=2)
grain(img,0.04).save("misty_mountains.jpg","JPEG",quality=82)

# 3. Sea sparkle
random.seed(37)
img = grad([(0,(150,200,235)),(0.42,(190,222,240)),(0.45,(52,110,160)),(0.75,(38,92,142)),(1,(30,74,120))])
d = ImageDraw.Draw(img,"RGBA")
for i in range(5):
    cy = 60+i*24; cx = random.randint(60,W-260)
    d.ellipse([cx,cy,cx+random.randint(120,240),cy+random.randint(10,20)], fill=(255,255,255,random.randint(60,110)))
img = img.filter(ImageFilter.GaussianBlur(1))
d = ImageDraw.Draw(img,"RGBA")
horizon = int(0.44*H); sunx = 560
for y in range(horizon+6, H, 3):
    spread = int(10 + (y-horizon)*0.55)
    for _ in range(3):
        x = sunx + random.randint(-spread,spread)
        w2 = random.randint(4,14)
        d.ellipse([x,y,x+w2,y+2], fill=(255,244,200,random.randint(90,200)))
for y in range(horizon+10, H, 14):
    x0 = random.randint(0,120)
    while x0 < W:
        ln = random.randint(30,90)
        d.line([(x0,y+random.randint(-2,2)),(x0+ln,y+random.randint(-2,2))], fill=(255,255,255,36), width=1)
        x0 += ln + random.randint(40,140)
sb = ImageDraw.Draw(img)
sb.polygon([(150,horizon-2),(166,horizon-26),(170,horizon-2)], fill=(250,250,252))
sb.line([(150,horizon-2),(170,horizon-2)], fill=(90,90,100), width=2)
grain(img,0.045).save("sea_sparkle.jpg","JPEG",quality=82)

for f in ("starry_night","misty_mountains","sea_sparkle"):
    print(f, os.path.getsize(f + ".jpg"))
