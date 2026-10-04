#!/usr/bin/env python3
"""Render the eye artwork (emoji-style eyes) -> main/img_eyes.c / main/img_eyes.h (LVGL ARGB8888).

  sclera : the white of the eye, an oval with a soft grey rim and inner shading
  iris   : round iris with limbal ring, light crescent at the bottom, dark pupil and a glint; one per mood colour
Usage: python tools/gen_eyes.py [--preview out.png]     (needs pillow)
"""
import math
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 4                      # supersampling for smooth edges
EYE_W, EYE_H = 88, 150
IRIS = 50

# name: (outer ring, mid colour, light crescent, pupil)
THEMES = {
    "idle":       ((98, 50, 12),  (150, 84, 16),  (214, 150, 26),  (46, 20, 4)),      # warm brown, like the reference
    "listening":  ((10, 88, 62),  (24, 168, 112), (120, 240, 186), (4, 38, 28)),
    "thinking":   ((66, 28, 118), (128, 70, 208), (206, 156, 255), (24, 10, 48)),
    "speaking":   ((138, 58, 0),  (232, 130, 10), (255, 214, 96),  (58, 24, 0)),
    "connecting": ((10, 68, 120), (30, 140, 222), (146, 224, 255), (4, 28, 54)),
    "error":      ((108, 10, 10), (204, 42, 42),  (255, 138, 112), (44, 4, 4)),
    "boot":       ((58, 68, 98),  (120, 134, 176), (204, 214, 238), (18, 22, 34)),
}


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def smooth(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def sclera():
    w, h = EYE_W * SS, EYE_H * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    cx, cy, a, b = w / 2, h / 2, w / 2 - 2 * SS, h / 2 - 2 * SS
    for y in range(h):
        for x in range(w):
            d = math.hypot((x - cx) / a, (y - cy) / b)
            if d > 1.0:
                continue
            ny = (y - cy) / b                                   # -1 top .. +1 bottom
            shade = smooth(0.70, 1.0, d) ** 1.4                 # darker towards the rim
            top = smooth(0.0, 1.0, -ny) * 0.5 * smooth(0.45, 1.0, d)   # extra shadow under the upper lid
            g = 255 - int(70 * shade) - int(26 * top)
            px[x, y] = (g, g, min(255, g + 3), 255)
    # grey rim
    rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rim).ellipse([2 * SS, 2 * SS, w - 2 * SS, h - 2 * SS], outline=(150, 150, 156, 255), width=int(2.2 * SS))
    img = Image.alpha_composite(img, rim)
    return img.resize((EYE_W, EYE_H), Image.LANCZOS)


def iris(theme):
    outer, mid, light, pupil = theme
    n = IRIS * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = n / 2
    R = c - 1 * SS
    d.ellipse([c - R, c - R, c + R, c + R], fill=outer + (255,))            # limbal ring
    # body: vertical gradient mid (top) -> light (bottom), clipped to a slightly smaller disc
    body = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bp = body.load()
    r2 = R * 0.86
    for y in range(n):
        for x in range(n):
            dd = math.hypot(x - c, y - (c - 0.02 * R))
            if dd <= r2:
                t = smooth(-0.2, 1.0, (y - (c - r2)) / (2 * r2)) ** 1.6      # the light colour gathers at the bottom
                col = lerp(mid, light, t * 0.85)
                edge = smooth(0.78, 1.0, dd / r2)                           # darken a little towards the ring
                bp[x, y] = lerp(col, outer, edge * 0.45) + (255,)
    img = Image.alpha_composite(img, body)
    d = ImageDraw.Draw(img)
    pr = R * 0.50                                                          # pupil
    d.ellipse([c - pr, c - 0.03 * R - pr, c + pr, c - 0.03 * R + pr], fill=pupil + (255,))
    # glint: tilted white oval at the upper left
    g = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    gw, gh = R * 0.30, R * 0.40
    ImageDraw.Draw(g).ellipse([c - R * 0.40 - gw, c - R * 0.42 - gh, c - R * 0.40 + gw, c - R * 0.42 + gh], fill=(255, 255, 255, 250))
    g = g.rotate(28, center=(c - R * 0.40, c - R * 0.42), resample=Image.BICUBIC)
    img = Image.alpha_composite(img, g)
    # small secondary glint, lower right
    g2 = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(g2).ellipse([c + R * 0.30, c + R * 0.30, c + R * 0.46, c + R * 0.46], fill=(255, 255, 255, 110))
    img = Image.alpha_composite(img, g2)
    return img.resize((IRIS, IRIS), Image.LANCZOS)


def c_array(name, img):
    r, g, b, a = img.split()
    raw = Image.merge("RGBA", (b, g, r, a)).tobytes()                       # LVGL ARGB8888 is B,G,R,A in memory
    rows = ["    " + ",".join(f"0x{v:02x}" for v in raw[i:i + 24]) + "," for i in range(0, len(raw), 24)]
    w, h = img.size
    return (f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            f"    .header.magic = LV_IMAGE_HEADER_MAGIC,\n    .header.cf = LV_COLOR_FORMAT_ARGB8888,\n"
            f"    .header.w = {w},\n    .header.h = {h},\n    .header.stride = {w * 4},\n"
            f"    .data_size = sizeof({name}_map),\n    .data = {name}_map,\n}};\n")


def main():
    eye = sclera()
    irises = {k: iris(v) for k, v in THEMES.items()}
    c = ["// Generated by tools/gen_eyes.py - do not edit.", '#include "img_eyes.h"', "", c_array("img_eye_sclera", eye)]
    h = ["// Generated by tools/gen_eyes.py - do not edit.", "#pragma once", '#include "lvgl.h"', "",
         f"#define EYE_W {EYE_W}", f"#define EYE_H {EYE_H}", f"#define IRIS_D {IRIS}", "",
         "extern const lv_image_dsc_t img_eye_sclera;"]
    for k, im in irises.items():
        c.append(c_array(f"img_iris_{k}", im))
        h.append(f"extern const lv_image_dsc_t img_iris_{k};")
    open("main/img_eyes.c", "w").write("\n".join(c))
    open("main/img_eyes.h", "w").write("\n".join(h) + "\n")
    print("sclera", eye.size, "| irises:", ", ".join(irises))
    if "--preview" in sys.argv:
        path = sys.argv[sys.argv.index("--preview") + 1]
        S = 2
        W = H = 240
        sheet = Image.new("RGB", (W * S * 3 + 40, H * S), (5, 7, 15))
        scenes = [("idle", (-0.5, 0.45), 1.0), ("listening", (0, 0), 1.08), ("thinking", (0.75, -0.8), 0.92)]
        for i, (name, (gx, gy), op) in enumerate(scenes):
            scr = Image.new("RGBA", (W * S, H * S), (5, 7, 15, 255))
            for ex in (26, 126):
                e = eye.resize((int(EYE_W * S), int(EYE_H * S * op)), Image.LANCZOS)
                ey = int((118 - EYE_H * op / 2) * S)
                scr.alpha_composite(e, (ex * S, ey))
                ir = irises[name].resize((IRIS * S, IRIS * S), Image.LANCZOS)
                icx = ex + EYE_W / 2 + gx * 17
                icy = 118 + gy * 40 * op
                scr.alpha_composite(ir, (int((icx - IRIS / 2) * S), int((icy - IRIS / 2) * S)))
            sheet.paste(scr.convert("RGB"), (i * (W * S + 20), 0))
        sheet.save(path)


main()
