"""Previews the flat mango for kitchensink's title bar and prints its
points (fruit and leaf, about -1..1) for the C++ code."""
import math, sys
from PIL import Image, ImageDraw

def fruit(n=28):
    pts = []
    for i in range(n):
        th = 2 * math.pi * i / n
        x, y = 0.60 * math.cos(th), 0.80 * math.sin(th)
        # Flatter on the right, a kidney's inner curve.
        x -= 0.13 * max(0.0, math.cos(th)) ** 2 * (1 - abs(math.sin(th)))
        # A little fuller at the bottom than the top.
        x *= 1.0 + 0.10 * math.sin(th)
        a = math.radians(-28)
        pts.append((x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a) + 0.12))
    return pts

def leaf(n=10):
    # An almond from the stem, up and to the right.
    base = (0.25, -0.60)
    tip = (0.88, -0.88)
    dx, dy = tip[0] - base[0], tip[1] - base[1]
    L = math.hypot(dx, dy); ux, uy = dx / L, dy / L; nx, ny = -uy, ux
    pts = []
    for side in (1, -1):
        rng = range(n + 1) if side == 1 else range(n, -1, -1)
        for i in rng:
            t = i / n
            w = 0.17 * math.sin(math.pi * t) ** 0.9
            pts.append((base[0] + ux * L * t + nx * w * side, base[1] + uy * L * t + ny * w * side))
    return pts

def render(size, path, colour=(0x8C, 0x92, 0x9D)):
    S = size * 8
    img = Image.new('RGB', (S, S), (0x1A, 0x1D, 0x23)); d = ImageDraw.Draw(img)
    r = S * 0.44
    tr = lambda p: (S / 2 + p[0] * r, S / 2 + p[1] * r)
    d.polygon([tr(p) for p in fruit()], fill=colour)
    d.polygon([tr(p) for p in leaf()], fill=colour)
    img.resize((size, size), Image.LANCZOS).resize((size * 4, size * 4), Image.NEAREST).save(path)

if __name__ == '__main__':
    out = sys.argv[1]
    render(26, out + '/flat26.png')
    render(64, out + '/flat64.png')
    print('fruit', [(round(x, 3), round(y, 3)) for x, y in fruit(24)])
    print('leaf', [(round(x, 3), round(y, 3)) for x, y in leaf(6)])
