import os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageFilter

W, H = 800, 480
SRCS = [
    (r"D:\Downloads\012\Weixin Image_20260830063301_12_6845.png", "shrine.jpg"),
    (r"D:\Downloads\012\Weixin Image_20260830063315_13_6845.jpg", "doodle.jpg"),
    (r"D:\Downloads\012\Weixin Image_20260830063300_11_6845.png", "pixel.jpg"),
]

for src, out in SRCS:
    im = Image.open(src).convert("RGB")
    sw, sh = im.size
    scale_cover = max(W / sw, H / sh)
    scale_fit = min(W / sw, H / sh)
    if scale_cover / scale_fit < 1.25:
        # Aspect close to 5:3 -> plain center-crop cover.
        nw, nh = int(sw * scale_cover + 0.5), int(sh * scale_cover + 0.5)
        im2 = im.resize((nw, nh), Image.LANCZOS)
        x0, y0 = (nw - W) // 2, (nh - H) // 2
        canvas = im2.crop((x0, y0, x0 + W, y0 + H))
    else:
        # Portrait / mismatched: blurred cover backdrop + fitted foreground.
        nw, nh = int(sw * scale_cover + 0.5), int(sh * scale_cover + 0.5)
        bg = im.resize((nw, nh), Image.LANCZOS)
        x0, y0 = (nw - W) // 2, (nh - H) // 2
        bg = bg.crop((x0, y0, x0 + W, y0 + H)).filter(ImageFilter.GaussianBlur(18))
        bg = bg.point(lambda p: int(p * 0.72))
        fw, fh = int(sw * scale_fit + 0.5), int(sh * scale_fit + 0.5)
        fg = im.resize((fw, fh), Image.LANCZOS)
        bg.paste(fg, ((W - fw) // 2, (H - fh) // 2))
        canvas = bg
    canvas.save(out, "JPEG", quality=85, optimize=True, progressive=False)
    print(out, os.path.getsize(out), "src", sw, "x", sh)
