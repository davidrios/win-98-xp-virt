#!/usr/bin/env python3
"""motoracer-state.py <screendump.ppm> — which Moto Racer 1997 screen a
640x480 screendump shows (tools/xp-motoracer.sh drives the menus by it).

Prints one of: title, name, menu, mode, race-select, showroom, demo-menu,
other. The game's screens are told apart by a few pixels each (the yellow
header band of the 2D menus, the showroom's spotlights, the title's flames);
anything else — a race, the attract demo, a loading screen — is "other".
"""
import sys


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: magic, width height, maxval, one whitespace, then the pixels
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:]


def px(img, x, y):
    w, h, pix = img
    if x >= w or y >= h:
        return (0, 0, 0)
    o = (y * w + x) * 3
    return pix[o], pix[o + 1], pix[o + 2]


def near(c, ref, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def state(img):
    p = lambda x, y: px(img, x, y)
    header = near(p(550, 65), (255, 239, 0), 30)                     # the yellow band of the 2D menus
    if header and near(p(450, 373), (214, 150, 8), 40) and near(p(100, 100), (82, 81, 214), 50):
        return "showroom"                                            # CHOOSE BIKE: the A-T button under a spotlight
    if near(p(320, 30), (255, 255, 255), 20) and near(p(30, 450), (8, 8, 33), 30):
        return "showroom"                                            # the same without its 2D panels (the executor before the untracked-write merge)
    if near(p(400, 65), (181, 166, 0), 40) and near(p(100, 420), (148, 109, 239), 50):
        return "name"                                                # ENTER YOUR NAME: purple letters under the band
    if header and near(p(110, 260), (255, 255, 24), 40):
        return "mode"                                                # SELECT MODE: the yellow tyre of Practice
    if header and near(p(100, 420), (0, 16, 8), 30) and near(p(565, 372), (82, 81, 189), 40):
        return "race-select"                                         # SELECT RACE: the map panel, the Continue button
    if header and near(p(165, 255), (107, 138, 74), 50) and near(p(100, 420), (66, 65, 181), 40):
        return "menu"                                                # MAIN MENU: the Solo panel over the blue checker
    if near(p(550, 65), (165, 40, 33), 40) and near(p(100, 420), (173, 199, 247), 40):
        return "title"                                               # flames top right, the blue bike bottom left
    return "other"


if __name__ == "__main__":
    print(state(load_ppm(sys.argv[1])))
