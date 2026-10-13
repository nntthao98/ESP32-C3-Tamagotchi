
from pathlib import Path
from PIL import Image

# ==========================================
# CONFIG
# ==========================================

ROOT = Path(__file__).resolve().parent
PNG_DIR = ROOT / "PNG"
OUTPUT = ROOT / "assets.h"

FRAMES = [
    "PET_IDLE_01",
    "PET_IDLE_MOVE",
    "PET_IDLE_HAPPY",
    "PET_IDLE_HAPPY_MOVE",
    "PET_IDLE_BORED",
    "PET_IDLE_BORED_MOVE",
    "PET_IDLE_HUNGRY",
    "PET_IDLE_HUNGRY_MOVE",
    "PET_IDLE_SLEEP",
    "PET_IDLE_SLEEP_MOVE",
]

WIDTH = 48
HEIGHT = 48
PIXEL_COUNT = WIDTH * HEIGHT


# ==========================================
# RGB888 -> RGB565
# ==========================================

def rgb565(r, g, b):
    return (
        ((r & 0xF8) << 8)
        | ((g & 0xFC) << 3)
        | (b >> 3)
    )


# ==========================================
# FORMAT C ARRAY
# ==========================================

def format_array(values, per_line=12):
    lines = []

    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append(
            "  " + ", ".join(f"0x{v:04X}" for v in chunk)
        )

    return ",\n".join(lines)


def format_alpha(values, per_line=16):
    lines = []

    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append(
            "  " + ", ".join(str(v) for v in chunk)
        )

    return ",\n".join(lines)


# ==========================================
# CHECK INPUT FILES BEFORE GENERATING
# ==========================================

if not PNG_DIR.is_dir():
    raise SystemExit(
        f"ERROR: Khong tim thay thu muc PNG:\n{PNG_DIR}"
    )

missing = [
    name for name in FRAMES
    if not (PNG_DIR / f"{name}.png").is_file()
]

if missing:
    raise SystemExit(
        "ERROR: Thieu cac file PNG:\n"
        + "\n".join(missing)
        + "\n\nHay kiem tra ten file va thu muc PNG."
    )


# ==========================================
# GENERATE ASSETS HEADER
# ==========================================

out = [
    "// Tamagotchi pet sprite assets",
    "// Generated from 10 PNG frames: RGB565 + alpha",
    "#pragma once",
    "#include <Arduino.h>",
    "",
    "#define PET_SPRITE_W 48",
    "#define PET_SPRITE_H 48",
    "#define PET_SPRITE_PIXELS (PET_SPRITE_W * PET_SPRITE_H)",
    "",
]

for name in FRAMES:
    path = PNG_DIR / f"{name}.png"

    with Image.open(path) as source:
        image = source.convert("RGBA")

    if image.size != (WIDTH, HEIGHT):
        raise SystemExit(
            f"ERROR: {name}.png co kich thuoc "
            f"{image.width}x{image.height}, can 48x48."
        )

    pixels = []
    alpha_values = []

    for r, g, b, a in image.getdata():
        pixels.append(rgb565(r, g, b))
        alpha_values.append(a)

    if len(pixels) != PIXEL_COUNT:
        raise SystemExit(
            f"ERROR: So pixel khong hop le: {name}"
        )

    out.extend([
        "",
        f"// {name}: {WIDTH}x{HEIGHT}",
        f"const uint16_t {name}_pixels[PET_SPRITE_PIXELS] PROGMEM = {{",
        format_array(pixels),
        "};",
        "",
        f"const uint8_t {name}_alpha[PET_SPRITE_PIXELS] PROGMEM = {{",
        format_alpha(alpha_values),
        "};",
    ])


# ==========================================
# COMPATIBLE WITH OLD FIRMWARE
# ==========================================

out.extend([
    "",
    "enum PetSpriteId {",
])

for index, name in enumerate(FRAMES):
    out.append(f"  SPRITE_{name} = {index},")

out.extend([
    "};",
    "",
    "struct PetSprite {",
    "  const uint16_t* pixels;",
    "  const uint8_t* alpha;",
    "};",
    "",
    "const PetSprite PET_SPRITES[10] = {",
])

for name in FRAMES:
    out.append(
        f"  {{ {name}_pixels, {name}_alpha }},"
    )

out.extend([
    "};",
    "",
])


# ==========================================
# WRITE FILE SAFELY
# ==========================================

content = "\n".join(out)

# Ghi file tam truoc, tranh lam mat file cu neu co loi ghi.
temp_output = ROOT / "assets_temp.h"
temp_output.write_text(content, encoding="utf-8")

temp_output.replace(OUTPUT)

print("====================================")
print("Tao assets.h thanh cong!")
print("So frame:", len(FRAMES))
print("Kich thuoc moi frame: 48 x 48")
print("Dinh dang: RGB565 + alpha 8-bit")
print("Cau truc: PetSprite / PET_SPRITES")
print("File:", OUTPUT)
print("====================================")
