#!/usr/bin/env python3
"""
M5_NightscoutMon Framebuffer Simulator & Preview Generator
Renders 320x240 pixel-perfect simulated screens using Pillow.
Usage:
    python3 tools/emulator/render_preview.py --page 5 --dose 1.5 --type fast --output preview.png
"""

import argparse
import os
import sys
from PIL import Image, ImageDraw, ImageFont

WIDTH = 320
HEIGHT = 240

# Color definitions matching M5_NightscoutMon TFT colors
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
DARKGREY = (51, 65, 85)
LIGHTGREY = (148, 163, 184)
CYAN = (56, 189, 248)
GREEN = (34, 197, 94)
YELLOW = (234, 179, 8)
RED = (239, 68, 68)
MAGENTA = (192, 132, 252)

# Slate container backgrounds
CARD_BG = (9, 14, 23)
BORDER_COLOR = (30, 41, 59)
BTN_BG = (30, 41, 59)
FAST_ACT_BG = (15, 58, 122)
LONG_ACT_BG = (88, 28, 135)


def get_font(size, bold=False):
    # Try system fonts on macOS / Linux
    font_paths = [
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/SFPro.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    ]
    for p in font_paths:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def draw_header(draw, title, sgv=95, trend="Flat", time_str="02:15 AM"):
    # Header line
    draw.line([(0, 22), (319, 22)], fill=DARKGREY, width=1)

    f_title = get_font(11, bold=True)
    draw.text((8, 5), title, fill=CYAN, font=f_title)

    # SGV
    f_sgv = get_font(12, bold=True)
    sgv_col = GREEN if (70 <= sgv <= 180) else (YELLOW if (54 <= sgv <= 250) else RED)
    arrow = " \u2192" if trend == "Flat" else (" \u2191" if "Up" in trend else " \u2193")
    draw.text((135, 4), f"{sgv}{arrow}", fill=sgv_col, font=f_sgv)

    # Time
    f_time = get_font(11)
    draw.text((260, 5), time_str, fill=LIGHTGREY, font=f_time)


def draw_bottom_bar(draw):
    draw.line([(0, 216), (319, 216)], fill=DARKGREY, width=1)
    # Sun icon
    draw.ellipse([(49, 224), (57, 232)], outline=LIGHTGREY, width=1)
    # Clock icon
    draw.ellipse([(154, 223), (166, 235)], outline=LIGHTGREY, width=1)
    draw.line([(160, 225), (160, 229)], fill=LIGHTGREY, width=1)
    draw.line([(160, 229), (163, 229)], fill=LIGHTGREY, width=1)
    # Door icon
    draw.rectangle([(260, 222), (270, 236)], outline=LIGHTGREY, width=1)


def draw_round_box(draw, xy, fill, outline, radius=5):
    draw.rounded_rectangle(xy, radius=radius, fill=fill, outline=outline, width=1)


def render_page_5(dose=1.0, is_fast=True, sgv=95, output_file="page5_preview.png"):
    img = Image.new("RGB", (WIDTH, HEIGHT), BLACK)
    draw = ImageDraw.Draw(img)

    draw_header(draw, "LOG INSULIN", sgv=sgv)

    # 1. Mode Toggle (y: 28..58)
    f_tab = get_font(11, bold=True)
    fast_bg = FAST_ACT_BG if is_fast else CARD_BG
    fast_border = CYAN if is_fast else BORDER_COLOR
    fast_text = WHITE if is_fast else LIGHTGREY
    draw_round_box(draw, [(10, 28), (155, 58)], fill=fast_bg, outline=fast_border)
    draw.text((45, 36), "Fast Acting", fill=fast_text, font=f_tab)

    long_bg = LONG_ACT_BG if not is_fast else CARD_BG
    long_border = MAGENTA if not is_fast else BORDER_COLOR
    long_text = WHITE if not is_fast else LIGHTGREY
    draw_round_box(draw, [(165, 28), (310, 58)], fill=long_bg, outline=long_border)
    draw.text((198, 36), "Long Acting", fill=long_text, font=f_tab)

    # 2. Stepper Card (y: 66..136)
    draw_round_box(draw, [(10, 66), (310, 136)], fill=CARD_BG, outline=BORDER_COLOR, radius=8)

    # Stepper Buttons
    f_btn = get_font(11, bold=True)
    if is_fast:
        draw_round_box(draw, [(15, 72), (59, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((23, 93), "-1.0", fill=WHITE, font=f_btn)
        draw_round_box(draw, [(63, 72), (107, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((71, 93), "-0.5", fill=WHITE, font=f_btn)
    else:
        draw_round_box(draw, [(15, 72), (59, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((27, 93), "-5", fill=WHITE, font=f_btn)
        draw_round_box(draw, [(63, 72), (107, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((75, 93), "-1", fill=WHITE, font=f_btn)

    # Large Center Dose
    f_dose = get_font(22, bold=True)
    dose_str = f"{dose:.1f} U" if is_fast else f"{int(dose)} U"
    dose_col = CYAN if is_fast else MAGENTA
    draw.text((128, 76), dose_str, fill=dose_col, font=f_dose)

    # Suggestion tag
    if is_fast:
        sug = max(0.5, round(((sgv - 100) / 45.0) * 2.0) / 2.0) if sgv >= 120 else 0.5
        draw_round_box(draw, [(128, 108), (192, 130)], fill=(6, 78, 59), outline=GREEN, radius=4)
        f_sug = get_font(10, bold=True)
        draw.text((138, 112), f"~{sug:.1f}U", fill=GREEN, font=f_sug)

    # Right Stepper Buttons
    if is_fast:
        draw_round_box(draw, [(213, 72), (257, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((220, 93), "+0.5", fill=WHITE, font=f_btn)
        draw_round_box(draw, [(261, 72), (305, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((268, 93), "+1.0", fill=WHITE, font=f_btn)
    else:
        draw_round_box(draw, [(213, 72), (257, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((226, 93), "+1", fill=WHITE, font=f_btn)
        draw_round_box(draw, [(261, 72), (305, 130)], fill=BTN_BG, outline=DARKGREY)
        draw.text((272, 93), "+5", fill=WHITE, font=f_btn)

    # 3. Action Button (y: 146..204)
    act_bg = FAST_ACT_BG if is_fast else LONG_ACT_BG
    act_border = CYAN if is_fast else MAGENTA
    draw_round_box(draw, [(10, 146), (310, 204)], fill=act_bg, outline=act_border, radius=8)
    f_act = get_font(13, bold=True)
    act_text = f"LOG {dose:.1f} U FAST ACTING" if is_fast else f"LOG {int(dose)} U LONG ACTING"
    draw.text((46 if is_fast else 44, 166), act_text, fill=WHITE, font=f_act)

    draw_bottom_bar(draw)

    img.save(output_file)
    print(f"Rendered screen saved to: {output_file}")


def main():
    parser = argparse.ArgumentParser(description="M5_NightscoutMon Screen Preview Generator")
    parser.add_argument("--page", type=int, default=5, help="Page index (0-7)")
    parser.add_argument("--dose", type=float, default=1.0, help="Insulin dose (e.g. 1.5)")
    parser.add_argument("--type", choices=["fast", "long"], default="fast", help="Insulin type")
    parser.add_argument("--sgv", type=int, default=95, help="Current SGV in mg/dL")
    parser.add_argument("--output", default="screen_preview.png", help="Output PNG filepath")
    args = parser.parse_args()

    if args.page == 5:
        render_page_5(dose=args.dose, is_fast=(args.type == "fast"), sgv=args.sgv, output_file=args.output)
    else:
        print(f"Page {args.page} preview generator ready.")


if __name__ == "__main__":
    main()
