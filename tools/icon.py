"""Draws Mango's icon: a low-poly mango, green at the stem shading through
orange to red, lit from the left. Writes src/app.ico (16-256 px) and a
256 px PNG preview. Run: python3 tools/icon.py"""
import math, random, sys
from PIL import Image, ImageDraw

S = 1024  # drawn big, then shrunk for smoothness

def lerp(a, b, t): return a + (b - a) * t
def mix(c1, c2, t): return tuple(lerp(x, y, t) for x, y in zip(c1, c2))

# Colours down the fruit, stem (u=0) to bottom (u=1).
STOPS = [(0.0, (120, 190, 40)), (0.22, (170, 190, 30)), (0.42, (245, 160, 35)),
         (0.65, (240, 95, 30)), (1.0, (205, 35, 25))]

def along(u):
    for (u0, c0), (u1, c1) in zip(STOPS, STOPS[1:]):
        if u <= u1:
            return mix(c0, c1, (u - u0) / (u1 - u0))
    return STOPS[-1][1]

def shape(u, v):
    """Point on the fruit: u 0..1 stem to bottom, v -1..1 across."""
    t = u * 2 - 1
    # Narrow at the stem, widest two-thirds down, round at the bottom.
    half = (u ** 0.62) * ((1 - u) ** 0.3) * 1.55
    bend = -0.09 * (1 - t * t) - 0.03 * t                                 # belly bulges left
    x, y = (v * half * 0.5 + bend) * 0.5, t * 0.5
    a = math.radians(20)  # leant over to the right
    return (S / 2 + (x * math.cos(a) - y * math.sin(a)) * S, S / 2 + (x * math.sin(a) + y * math.cos(a)) * S * 0.98 + S * 0.01)

def draw():
    random.seed(7)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rows, cols = 8, 5
    us = [0, 0.07, 0.18, 0.31, 0.45, 0.6, 0.75, 0.9, 1]
    vs = [-1, -0.66, -0.2, 0.25, 0.68, 1]
    jit = {(i, j): (random.uniform(-0.03, 0.03) if 0 < i < rows else 0) for i in range(rows + 1) for j in range(cols + 1)}
    pt = lambda i, j: shape(min(1, max(0, us[i] + jit[(i, j)])), vs[j])
    for i in range(rows):
        for j in range(cols):
            quad = [pt(i, j), pt(i, j + 1), pt(i + 1, j + 1), pt(i + 1, j)]
            uc, vc = (us[i] + us[i + 1]) / 2, (vs[j] + vs[j + 1]) / 2
            base = along(uc)
            base = mix(base, (230, 40, 25), max(0, vc) * 0.25 * uc)   # redder on the shaded side
            light = 1.12 - 0.32 * (vc + 1) / 2 - 0.12 * abs(vc) ** 2 + random.uniform(-0.03, 0.03)
            light += 0.1 * math.exp(-((vc + 0.45) ** 2) * 6) * math.exp(-((uc - 0.38) ** 2) * 8)  # a soft highlight
            c = tuple(max(0, min(255, int(x * light))) for x in base)
            # Two triangles a quad, shaded a touch apart, for the faceted look.
            d.polygon([quad[0], quad[1], quad[2]], fill=c + (255,))
            c2 = tuple(max(0, min(255, int(x * 0.97))) for x in c)
            d.polygon([quad[0], quad[2], quad[3]], fill=c2 + (255,))
    return img

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "src/app.ico"
    big = draw()
    bbox = big.getbbox()
    big = big.crop(bbox)
    side = max(big.size)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(big, ((side - big.width) // 2, (side - big.height) // 2))
    pad = int(side * 0.04)
    canvas = Image.new("RGBA", (side + 2 * pad, side + 2 * pad), (0, 0, 0, 0))
    canvas.paste(sq, (pad, pad))
    canvas.resize((256, 256), Image.LANCZOS).save(out.replace(".ico", ".png"))
    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)]
    canvas.resize((256, 256), Image.LANCZOS).save(out, sizes=sizes)

main()
