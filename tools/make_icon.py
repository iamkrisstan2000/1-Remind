"""Generate the 1-Remind! Fluent-style app icon:
A layered Windows 11 Fluent rounded tile with a sleek hourglass and a small '1' badge.

Rendered at 4x (1024x1024) and downsampled with LANCZOS for crisp anti-aliased edges,
then saved as a multi-size .ico (16..256) and a 256px PNG preview.
"""
import math
from PIL import Image, ImageDraw, ImageFilter

SS = 4
N = 256 * SS  # 1024


def lerp(a, b, t):
    return a + (b - a) * t


def lerp_color(c1, c2, t):
    return tuple(int(round(lerp(c1[i], c2[i], t))) for i in range(len(c1)))


def make_fluent_tile(n):
    """Create a Windows 11 Fluent squircle tile with diagonal gradient & specular edge."""
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    # Outer shadow for depth
    shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    pad = int(n * 0.04)
    rad = int(n * 0.225)
    sd.rounded_rectangle(
        [pad, pad + int(n * 0.015), n - pad, n - pad + int(n * 0.015)],
        radius=rad,
        fill=(6, 22, 55, 110),
    )
    shadow = shadow.filter(ImageFilter.GaussianBlur(int(n * 0.02)))
    img = Image.alpha_composite(img, shadow)

    # Diagonal Fluent gradient: bright azure/cyan top-left -> rich royal indigo bottom-right
    c_top = (36, 158, 255)
    c_mid = (18, 108, 224)
    c_bot = (15, 58, 156)

    grad = Image.new("RGBA", (n, n), (0, 0, 0, 255))
    px = grad.load()
    for y in range(n):
        ty = y / (n - 1)
        for x in range(n):
            tx = x / (n - 1)
            t = 0.65 * ty + 0.35 * tx
            if t < 0.5:
                c = lerp_color(c_top, c_mid, t * 2.0)
            else:
                c = lerp_color(c_mid, c_bot, (t - 0.5) * 2.0)
            px[x, y] = (*c, 255)

    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [pad, pad, n - pad - 1, n - pad - 1], radius=rad, fill=255
    )

    tile = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    tile.paste(grad, (0, 0), mask)

    # Subtle top-left glass highlight border
    hl = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    hld = ImageDraw.Draw(hl)
    hld.rounded_rectangle(
        [pad + int(n * 0.004), pad + int(n * 0.004), n - pad - int(n * 0.004), n - pad - int(n * 0.004)],
        radius=rad,
        outline=(255, 255, 255, 65),
        width=int(n * 0.008),
    )
    img = Image.alpha_composite(img, tile)
    img = Image.alpha_composite(img, hl)
    return img


def draw_hourglass_and_one(img):
    n = img.size[0]
    cx = n * 0.455
    cy = n * 0.50

    # Hourglass geometry
    top_y = n * 0.21
    bot_y = n * 0.79
    half_w = n * 0.215
    neck_w = n * 0.036

    # Drop shadow under hourglass
    sh_layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    shd = ImageDraw.Draw(sh_layer)
    sh_off = int(n * 0.015)
    shd.polygon(
        [
            (cx - half_w, top_y + sh_off),
            (cx + half_w, top_y + sh_off),
            (cx + neck_w, cy + sh_off),
            (cx + half_w, bot_y + sh_off),
            (cx - half_w, bot_y + sh_off),
            (cx - neck_w, cy + sh_off),
        ],
        fill=(4, 24, 68, 95),
    )
    sh_layer = sh_layer.filter(ImageFilter.GaussianBlur(int(n * 0.022)))
    img = Image.alpha_composite(img, sh_layer)

    # Smooth hourglass outer mask, inner cavity mask, and sand mask via exact polygon / scanline
    outer_pts_left = []
    outer_pts_right = []
    inner_top_pts = []
    inner_bot_pts = []

    y0 = int(top_y)
    y1 = int(bot_y)
    half_h = (y1 - y0) / 2.0

    def half_width_at(y):
        u = abs(y - cy) / half_h
        u = max(0.0, min(1.0, u))
        curve = math.sin(u * math.pi * 0.5) ** 0.78
        return neck_w + (half_w - neck_w) * curve

    for y in range(y0, y1 + 1):
        hw = half_width_at(y)
        outer_pts_left.append((cx - hw, y))
        outer_pts_right.append((cx + hw, y))

    outer_poly = outer_pts_left + list(reversed(outer_pts_right))

    wall = n * 0.038
    for y in range(y0, y1 + 1):
        hw = half_width_at(y)
        # Slope compensation so normal wall thickness stays uniform along the curve
        hw_next = half_width_at(min(y1, y + 2))
        slope = abs(hw_next - hw) / 2.0
        normal_wall = wall * math.sqrt(1.0 + slope * slope)
        ihw = max(0.0, hw - normal_wall)
        if y < cy - n * 0.010:
            inner_top_pts.append(((cx - ihw, y), (cx + ihw, y)))
        elif y > cy + n * 0.010:
            inner_bot_pts.append(((cx - ihw, y), (cx + ihw, y)))

    top_cavity_poly = [p[0] for p in inner_top_pts] + [p[1] for p in reversed(inner_top_pts)]
    bot_cavity_poly = [p[0] for p in inner_bot_pts] + [p[1] for p in reversed(inner_bot_pts)]

    # 1. Frosted inner glass cavity
    cavity_mask = Image.new("L", (n, n), 0)
    cd = ImageDraw.Draw(cavity_mask)
    if len(top_cavity_poly) >= 3:
        cd.polygon(top_cavity_poly, fill=255)
    if len(bot_cavity_poly) >= 3:
        cd.polygon(bot_cavity_poly, fill=255)

    glass_fill = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    gf_grad = Image.new("RGBA", (n, n), (210, 240, 255, 78))
    glass_fill.paste(gf_grad, (0, 0), cavity_mask)
    img = Image.alpha_composite(img, glass_fill)

    # 2. Golden Fluent sand inside the cavity
    sand_raw = Image.new("L", (n, n), 0)
    srd = ImageDraw.Draw(sand_raw)
    # Top sand level (lower half of upper bulb)
    top_sand_y = int(cy - half_h * 0.48)
    srd.rectangle([0, top_sand_y, n, int(cy)], fill=255)
    # Bottom sand mound
    mound_pts = []
    mound_top_y = cy + half_h * 0.34
    for x in range(int(cx - half_w), int(cx + half_w) + 1, 2):
        dx = abs(x - cx) / (half_w * 0.85)
        y_arch = mound_top_y + (half_h * 0.24) * (dx ** 1.45)
        mound_pts.append((x, y_arch))
    mound_poly = mound_pts + [(cx + half_w, y1), (cx - half_w, y1)]
    srd.polygon(mound_poly, fill=255)

    # Clip sand to the inner glass cavity + add falling stream through the neck
    sand_mask = Image.new("L", (n, n), 0)
    sand_mask.paste(sand_raw, (0, 0), cavity_mask)
    smd = ImageDraw.Draw(sand_mask)
    stream_w = n * 0.013
    smd.rounded_rectangle(
        [cx - stream_w, cy - n * 0.025, cx + stream_w, mound_top_y + n * 0.03],
        radius=int(stream_w),
        fill=255,
    )
    sand_mask = sand_mask.filter(ImageFilter.GaussianBlur(1.5))

    sand_layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sand_grad = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sg_px = sand_grad.load()
    s_top = (255, 226, 112, 255)
    s_bot = (255, 166, 34, 255)
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, (y1 - y0))
        c = lerp_color(s_top, s_bot, t)
        for x in range(int(cx - half_w - 10), int(cx + half_w + 10)):
            if 0 <= x < n:
                sg_px[x, y] = c
    sand_layer.paste(sand_grad, (0, 0), sand_mask)
    img = Image.alpha_composite(img, sand_layer)

    # 3. Razor-clean white hourglass walls (outer_poly minus cavity_mask) + rounded caps
    wall_mask = Image.new("L", (n, n), 0)
    wd = ImageDraw.Draw(wall_mask)
    wd.polygon(outer_poly, fill=255)
    if len(top_cavity_poly) >= 3:
        wd.polygon(top_cavity_poly, fill=0)
    if len(bot_cavity_poly) >= 3:
        wd.polygon(bot_cavity_poly, fill=0)

    # Top and bottom Fluent rounded caps
    cap_ext = n * 0.040
    cap_h = n * 0.056
    cap_r = int(cap_h * 0.48)
    wd.rounded_rectangle(
        [cx - half_w - cap_ext, top_y - cap_h * 0.65, cx + half_w + cap_ext, top_y + cap_h * 0.35],
        radius=cap_r,
        fill=255,
    )
    wd.rounded_rectangle(
        [cx - half_w - cap_ext, bot_y - cap_h * 0.35, cx + half_w + cap_ext, bot_y + cap_h * 0.65],
        radius=cap_r,
        fill=255,
    )
    wall_mask = wall_mask.filter(ImageFilter.GaussianBlur(1.2))
    white_layer = Image.new("RGBA", (n, n), (255, 255, 255, 255))
    frame_layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    frame_layer.paste(white_layer, (0, 0), wall_mask)
    img = Image.alpha_composite(img, frame_layer)

    # -----------------------------------------------------------------------
    # Small "1" Fluent badge on the bottom-right of the hourglass
    # -----------------------------------------------------------------------
    bx = n * 0.725
    by = n * 0.685
    br = n * 0.165

    # Badge shadow
    bsh = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bsh_d = ImageDraw.Draw(bsh)
    bsh_d.ellipse(
        [bx - br, by - br + n * 0.012, bx + br, by + br + n * 0.012],
        fill=(5, 20, 58, 150),
    )
    bsh = bsh.filter(ImageFilter.GaussianBlur(int(n * 0.018)))
    img = Image.alpha_composite(img, bsh)

    # Badge circle with crisp white ring + deep navy-indigo gradient
    badge = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bd = ImageDraw.Draw(badge)
    ring = n * 0.022
    bd.ellipse([bx - br - ring, by - br - ring, bx + br + ring, by + br + ring], fill=(255, 255, 255, 255))

    b_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(b_mask).ellipse([bx - br, by - br, bx + br, by + br], fill=255)
    b_grad = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bg_px = b_grad.load()
    bc_top = (14, 66, 170, 255)
    bc_bot = (7, 34, 102, 255)
    for y in range(int(by - br - 2), int(by + br + 3)):
        if 0 <= y < n:
            t = (y - (by - br)) / max(1.0, 2 * br)
            c = lerp_color(bc_top, bc_bot, max(0.0, min(1.0, t)))
            for x in range(int(bx - br - 2), int(bx + br + 3)):
                if 0 <= x < n:
                    bg_px[x, y] = c
    badge.paste(b_grad, (0, 0), b_mask)

    # Crisp geometric "1" inside the badge
    stem_w = br * 0.26
    num_top = by - br * 0.52
    num_bot = by + br * 0.52
    stem_x = bx + br * 0.04
    bd.rounded_rectangle(
        [stem_x - stem_w / 2, num_top, stem_x + stem_w / 2, num_bot],
        radius=int(stem_w * 0.45),
        fill=(255, 255, 255, 255),
    )
    bd.line(
        [(stem_x - br * 0.30, num_top + br * 0.24), (stem_x, num_top + stem_w * 0.35)],
        fill=(255, 255, 255, 255),
        width=int(stem_w * 0.92),
    )
    bd.ellipse(
        [
            stem_x - br * 0.30 - stem_w * 0.45,
            num_top + br * 0.24 - stem_w * 0.45,
            stem_x - br * 0.30 + stem_w * 0.45,
            num_top + br * 0.24 + stem_w * 0.45,
        ],
        fill=(255, 255, 255, 255),
    )
    base_w = br * 0.36
    bd.rounded_rectangle(
        [stem_x - base_w, num_bot - stem_w * 0.85, stem_x + base_w, num_bot],
        radius=int(stem_w * 0.42),
        fill=(255, 255, 255, 255),
    )

    img = Image.alpha_composite(img, badge)
    return img


img = make_fluent_tile(N)
img = draw_hourglass_and_one(img)

img256 = img.resize((256, 256), Image.LANCZOS)
img256.save(
    "src/app.ico",
    format="ICO",
    sizes=[
        (16, 16),
        (20, 20),
        (24, 24),
        (32, 32),
        (40, 40),
        (48, 48),
        (64, 64),
        (128, 128),
        (256, 256),
    ],
)
img256.save("src/app-preview.png")
print("1-Remind! icon written")
