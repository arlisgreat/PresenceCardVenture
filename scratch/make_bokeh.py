import os, random
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFilter
W,H = 800,480
random.seed(51)
img = Image.new("RGB",(W,H))
px = img.load()
for y in range(H):
    t = y/(H-1)
    for x in range(W):
        u = x/(W-1)
        r = int(58+120*t+30*u); g = int(30+60*t); b = int(46+40*(1-t))
        px[x,y] = (min(r,255),min(g,255),min(b,255))
lay = Image.new("RGBA",(W,H),(0,0,0,0)); d = ImageDraw.Draw(lay)
for _ in range(46):
    x,y = random.randint(-40,W), random.randint(-40,H)
    r = random.randint(18,70)
    warm = random.choice([(255,196,120),(255,160,110),(250,220,150),(240,140,130)])
    d.ellipse([x-r,y-r,x+r,y+r], fill=warm+(random.randint(28,80),))
lay = lay.filter(ImageFilter.GaussianBlur(10)); img.paste(lay,(0,0),lay)
lay2 = Image.new("RGBA",(W,H),(0,0,0,0)); d2 = ImageDraw.Draw(lay2)
for _ in range(18):
    x,y = random.randint(0,W), random.randint(0,H)
    r = random.randint(6,16)
    d2.ellipse([x-r,y-r,x+r,y+r], fill=(255,230,170,random.randint(90,160)))
lay2 = lay2.filter(ImageFilter.GaussianBlur(2)); img.paste(lay2,(0,0),lay2)
g2 = Image.effect_noise((W,H),14).convert("L")
img = Image.blend(img, Image.merge("RGB",(g2,g2,g2)), 0.05)
img.save("warm_bokeh.jpg","JPEG",quality=82)
print("warm_bokeh", os.path.getsize("warm_bokeh.jpg"))
