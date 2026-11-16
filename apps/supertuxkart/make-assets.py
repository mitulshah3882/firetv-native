#!/usr/bin/env python3
"""Trims SuperTuxKart 1.5's game data (774 MB) to fit a Fire TV stick, in the spirit of STK's own
android/generate_assets.sh but tuned for a 1080p TV:

  * 3D textures (textures/, tracks/, karts/, library/, models/) larger than 512 px are scaled
    down to 512. The game is configured to cap textures at 512 anyway (see src/vega_stk.cpp), so
    this changes nothing on screen. Menu art (gui/, skins/) keeps its full resolution.
  * Opaque PNG textures become JPEG (quality 88) when every file with that name is opaque and no
    code or binary model refers to the name; .spm meshes and XML files are updated to match
    (".png" and ".jpg" have the same length, so mesh strings are patched in place).
  * Textures of 3D models (every image an .spm mesh names, minus those STK also loads another
    way) are precompressed to ASTC 6x6 with their mipmaps, as "<image>.astc" (see
    src/vega_stk.cpp), and the image itself becomes a 4x4 stand-in: the GPU then samples
    compressed textures at a ninth of the memory, and loading skips the image decoding.
  * Music and sounds are re-encoded at a lower Vorbis quality by ogg-reencode (built from the
    pinned libogg/libvorbis sources), if it is available.
  * Dropped: desktop icons, the colour-emoji font (our FreeType is built without PNG support, so
    STK could not use it) and helper scripts. (Ghost replays stay: story-mode challenges race
    them, and STK refuses to start without data/replay.)

Every track, kart, song, translation and the licence/credit files are kept.

  make-assets.py <stk-data-dir> <out-dir> [--stk-src <dir>] [--ogg-tool <path>] [--astcenc <path>]

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import glob
import hashlib
import multiprocessing
import os
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image

MAX_TEX = 512
JPEG_QUALITY = 88
TEX_DIRS = ("textures", "tracks", "karts", "library", "models")
JPEG_DIRS = ("textures", "tracks", "karts", "library")
DROP_FILES = {"supertuxkart.icns", "supertuxkart_1024.png", "supertuxkart_512.png",
              "supertuxkart_256.png", "supertuxkart_128.png", "supertuxkart_64.png",
              "supertuxkart_48.png", "supertuxkart_32.png", "supertuxkart_16.png",
              "optimize_data.sh", "ttf/NotoColorEmoji.ttf"}
DROP_DIRS = set()
NEVER_JPEG = {"traffic_light.png"}  # also excluded by STK's Android script


def top_dir(rel):
    return rel.split("/", 1)[0]


def copy_tree(src, out):
    if os.path.exists(out):
        shutil.rmtree(out)
    for dirpath, dirnames, filenames in os.walk(src):
        rel_dir = os.path.relpath(dirpath, src)
        if rel_dir == ".":
            rel_dir = ""
        dirnames[:] = [d for d in dirnames if os.path.join(rel_dir, d) not in DROP_DIRS]
        os.makedirs(os.path.join(out, rel_dir), exist_ok=True)
        for name in filenames:
            rel = os.path.join(rel_dir, name)
            if rel in DROP_FILES or name.endswith(".py"):
                continue
            shutil.copy2(os.path.join(src, rel), os.path.join(out, rel))


def is_opaque(im):
    # A tRNS chunk makes a colour transparent in grey and RGB images too, not only in palette
    # ones: STK's "transparence" blockers are 2x2 black greyscale PNGs keyed transparent. Treating
    # them as opaque drew black boxes on the overworld, Mines, XR591 and Cocoa Temple.
    if "transparency" in im.info:
        return im.convert("RGBA").getchannel("A").getextrema()[0] == 255
    if im.mode in ("RGB", "L"):
        return True
    if im.mode in ("RGBA", "LA", "P", "PA"):
        if im.mode == "P" and "transparency" not in im.info:
            return True
        return im.convert("RGBA").getchannel("A").getextrema()[0] == 255
    return False


def scan_image(path):
    """(path, width, height, opaque, convertible-mode)"""
    try:
        with Image.open(path) as im:
            im.load()
            return path, im.width, im.height, is_opaque(im), im.mode in ("RGB", "RGBA", "L", "LA", "P", "PA")
    except Exception as e:  # noqa: BLE001 - keep odd files untouched
        print(f"  skip unreadable {path}: {e}", file=sys.stderr)
        return path, 0, 0, False, False


def process_image(job):
    """job = (path, to_jpeg). Downscale to MAX_TEX and/or convert to JPEG. Returns new path."""
    path, to_jpeg = job
    with Image.open(path) as im:
        im.load()
        fmt = im.format
        w, h = im.size
        resize = max(w, h) > MAX_TEX
        if not resize and not to_jpeg:
            return path
        if im.mode in ("P", "PA"):
            im = im.convert("RGBA" if not to_jpeg else "RGB")
        elif im.mode == "LA" and to_jpeg:
            im = im.convert("L")
        elif im.mode == "RGBA" and to_jpeg:
            im = im.convert("RGB")
        if resize:
            s = MAX_TEX / max(w, h)
            im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
        if to_jpeg:
            new = path[:-4] + ".jpg"
            im.save(new, "JPEG", quality=JPEG_QUALITY, optimize=True)
            os.remove(path)
            return new
        if fmt == "JPEG":
            im.save(path, "JPEG", quality=JPEG_QUALITY, optimize=True)
        else:
            im.save(path, "PNG", optimize=True)
        return path


def spm_textures(data):
    """Offsets/lengths of texture-name strings in an .spm mesh (see STK's sp_mesh_loader.cpp)."""
    if data[:2] != b"SP":
        return []
    (count,) = struct.unpack_from("<H", data, 28)
    pos, out = 30, []
    for _ in range(count * 2):
        n = data[pos]
        if n:
            out.append((pos + 1, n))
        pos += 1 + n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--ogg-tool")
    ap.add_argument("--stk-src", help="STK source root, to find texture names used by code")
    ap.add_argument("--astcenc", help="astcenc binary: precompress mesh textures to ASTC")
    args = ap.parse_args()
    src, out = os.path.abspath(args.src), os.path.abspath(args.out)

    print("copying data")
    copy_tree(src, out)
    # Upstream data bug: the overworld's track mesh uses metaldirt_diff.png, which ships only with
    # Lighthouse. STK looks in the track's own directory, so the surface drew black (the "black
    # rectangle" on the overworld's horizon). Give the overworld its own copy.
    for name in ("metaldirt_diff.png",):
        dst = os.path.join(out, "tracks", "overworld", name)
        if not os.path.exists(dst):
            shutil.copy2(os.path.join(out, "tracks", "lighthouse", name), dst)
    # Without the emoji font, stk_config.xml must not name it (STK treats a missing font as fatal).
    cfg = os.path.join(out, "stk_config.xml")
    with open(cfg, "r", encoding="utf-8") as f:
        text = f.read()
    with open(cfg, "w", encoding="utf-8") as f:
        f.write(text.replace('color-emoji-ttf="NotoColorEmoji.ttf"', 'color-emoji-ttf=""'))

    # Inventory.
    images, b3d_files, spm_files, text_files, oggs = [], [], [], [], []
    for dirpath, _, filenames in os.walk(out):
        for name in filenames:
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, out)
            low = name.lower()
            if low.endswith((".png", ".jpg", ".jpeg")) and top_dir(rel) in TEX_DIRS:
                images.append(p)
            elif low.endswith(".b3d"):
                b3d_files.append(p)
            elif low.endswith(".spm"):
                spm_files.append(p)
            elif low.endswith((".xml", ".stkgui", ".challenge", ".grandprix")):
                text_files.append(p)
            elif low.endswith(".ogg"):
                oggs.append(p)

    with multiprocessing.Pool() as pool:
        scans = pool.map(scan_image, images, chunksize=8)

    # Names that must stay PNG: used by code, by binary .b3d models, as gloss maps, or with alpha.
    keep = set(NEVER_JPEG)
    if args.stk_src:
        for sub in ("src", "lib/graphics_engine", "lib/irrlicht/source"):
            for dirpath, _, filenames in os.walk(os.path.join(args.stk_src, sub)):
                for name in filenames:
                    if name.endswith((".cpp", ".hpp", ".c", ".h")):
                        with open(os.path.join(dirpath, name), "rb") as f:
                            keep.update(m.decode() for m in re.findall(rb'([A-Za-z0-9_.-]+\.png)"', f.read()))
    for p in b3d_files:
        with open(p, "rb") as f:
            keep.update(m.decode() for m in re.findall(rb"([A-Za-z0-9_.-]+\.png)", f.read()))
    gloss = set()
    for p in text_files:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            gloss.update(re.findall(r'gloss-map="([^"]+)"', f.read()))
    keep_code = set(keep)  # names code or binary models load directly
    keep |= gloss
    for name in os.listdir(out):  # top-level config XMLs (powerups, stk_config...) by name
        if name.endswith(".xml"):
            with open(os.path.join(out, name), "r", encoding="utf-8", errors="replace") as f:
                keep.update(re.findall(r"([A-Za-z0-9_.-]+\.png)", f.read()))
    all_names = {}
    for p, w, h, opaque, ok in scans:
        all_names.setdefault(os.path.basename(p), []).append((p, opaque and ok))
    jpg_names = {os.path.basename(p)[:-4] for p in images if p.lower().endswith(".jpg")}
    to_jpeg = set()
    for name, files in all_names.items():
        if (name.endswith(".png") and name not in keep and name[:-4] not in jpg_names
                and all(op for _, op in files)
                and all(top_dir(os.path.relpath(p, out)) in JPEG_DIRS for p, _ in files)):
            to_jpeg.add(name)

    jobs = [(p, os.path.basename(p) in to_jpeg) for p, *_ in scans]
    print(f"images: {len(images)}, scaling >{MAX_TEX}px, {len(to_jpeg)} PNG names -> JPEG")
    with multiprocessing.Pool() as pool:
        pool.map(process_image, jobs, chunksize=4)

    # References: .spm texture strings (same length, patched in place) and text files.
    patched = 0
    for p in spm_files:
        with open(p, "rb") as f:
            data = bytearray(f.read())
        changed = False
        for off, n in spm_textures(data):
            name = data[off:off + n].decode("latin-1")
            base = name.rsplit("/", 1)[-1]
            if base in to_jpeg:
                data[off + n - 4:off + n] = b".jpg"
                changed = True
        if changed:
            with open(p, "wb") as f:
                f.write(data)
            patched += 1
    if to_jpeg:
        names = sorted(to_jpeg, key=len, reverse=True)
        pattern = re.compile(r'(?<=["\s/=])(' + "|".join(re.escape(n[:-4]) for n in names) + r')\.png(?=["\s<])')
        for p in text_files:
            with open(p, "r", encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
            new = pattern.sub(r"\1.jpg", text)
            if new != text:
                with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
                    f.write(new)
                patched += 1
    print(f"patched {patched} mesh/xml files")

    if args.astcenc and os.path.exists(args.astcenc):
        astc_textures(out, spm_files, text_files, keep_code, args.astcenc)

    if args.ogg_tool and os.path.exists(args.ogg_tool):
        print(f"re-encoding {len(oggs)} ogg files")
        with multiprocessing.Pool() as pool:
            pool.map(reencode_ogg, [(args.ogg_tool, p) for p in oggs], chunksize=1)


ASTC_BLOCK = (6, 6)


def pot_floor(n):
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def astc_one(job):
    """Compress one image (and its mipmaps) to "<image>.astc"; shrink the image to a stand-in.
    `bake` replays STK's SPTexture::getMask() (colorization or alpha mask into the alpha channel)
    because the compressed texture bypasses it at run time."""
    path, tool, tmpdir, bake = job
    try:
        with Image.open(path) as im:
            im.load()
            fmt = im.format
            img = im.convert("RGBA")
    except Exception:  # noqa: BLE001
        return path, 0
    # Same level-0 size as STK would upload: largest power of two <= size, at most MAX_TEX.
    w, h = min(pot_floor(img.width), MAX_TEX), min(pot_floor(img.height), MAX_TEX)
    level = img if (w, h) == img.size else img.resize((w, h), Image.LANCZOS)
    if bake:
        kind, mask_path, factor = bake
        if kind == "colorize":
            enc = max(0, min(255, int(factor * 0.4 * 255)))
            if mask_path:
                with Image.open(mask_path) as m:
                    ma = m.convert("RGBA").resize((w, h), Image.BILINEAR).getchannel("A")
                alpha = ma.point(lambda a: a if a > 127 else enc)
            else:
                alpha = Image.new("L", (w, h), enc)
        else:  # alpha mask: the mask's red channel
            with Image.open(mask_path) as m:
                alpha = m.convert("RGBA").resize((w, h), Image.BILINEAR).getchannel("R")
        level = level.copy()
        level.putalpha(alpha)
    blobs = []
    base = os.path.join(tmpdir, hashlib.md5(path.encode()).hexdigest())
    while True:
        src, dst = base + ".png", base + ".astc"
        level.save(src)
        r = subprocess.run([tool, "-cl", src, dst, f"{ASTC_BLOCK[0]}x{ASTC_BLOCK[1]}", "-medium",
                            "-j", "1", "-silent"], capture_output=True)
        if r.returncode != 0:
            return path, 0
        with open(dst, "rb") as f:
            blobs.append(f.read()[16:])  # skip the .astc file header
        if level.width == 1 and level.height == 1:
            break
        level = level.resize((max(1, level.width // 2), max(1, level.height // 2)), Image.LANCZOS)
    for f in (base + ".png", base + ".astc"):
        if os.path.exists(f):
            os.remove(f)
    with open(path + ".astc", "wb") as f:
        f.write(b"VAST" + struct.pack("<HHIII", ASTC_BLOCK[0], ASTC_BLOCK[1], w, h, len(blobs)))
        for b in blobs:
            f.write(b)
    stub = img.resize((4, 4), Image.LANCZOS)
    if fmt == "JPEG":
        stub.convert("RGB").save(path, "JPEG", quality=80)
    else:
        stub.save(path, "PNG")
    return path, len(blobs)


def alpha_shaders(out):
    """SP shaders that use the texture's alpha channel (STK skips colorization masks for them)."""
    names = set()
    for p in glob.glob(os.path.join(out, "shaders", "sps_*.xml")):
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        m = re.search(r'<spshader\b[^>]*>', text, re.S)
        if m and re.search(r'use-alpha-channel="Y"', m.group(0)):
            nm = re.search(r'name="([^"]+)"', m.group(0))
            if nm:
                names.add(nm.group(1))
    return names


def material_shader(attrs):
    """Material::Material's shader choice, including its backwards-compatible attributes."""
    if "shader" in attrs:
        return attrs["shader"]
    shader = "solid"
    yes = lambda k: attrs.get(k, "").upper() in ("Y", "TRUE", "1")
    if yes("additive") or yes("alpha"):
        shader = "alphablend"
    if yes("transparency"):
        shader = "alphatest"
    if attrs.get("light", "Y").upper() in ("N", "FALSE", "0"):
        shader = "unlit"
    comp = attrs.get("compositing")
    if comp == "blend":
        shader = "alphablend"
    elif comp in ("test", "coverage"):
        shader = "alphatest"
    elif comp == "additive":
        shader = "additive"
    return shader


def astc_textures(out, spm_files, text_files, keep, tool):
    alpha_sh = alpha_shaders(out)
    # Every material definition, by texture name (a name defined differently in two places is
    # resolved by context at run time, so it is only compressed if no definition needs a mask).
    materials = {}
    layer_names = set()
    mask_files = set()
    for p in text_files:
        if os.path.basename(p) != "materials.xml":
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in re.finditer(r"<material\s[^>]*>", text, re.S):
            attrs = dict(re.findall(r'([\w-]+)\s*=\s*"([^"]*)"', m.group(0)))
            if "name" not in attrs:
                continue
            materials.setdefault(attrs["name"], []).append((os.path.dirname(p), attrs))
            for k in ("gloss-map", "normal-map", "tex-layer-2", "tex-layer-3", "tex-layer-4",
                      "tex-layer-5"):
                if attrs.get(k):
                    layer_names.add(attrs[k])
            for k in ("mask", "colorization-mask"):
                if attrs.get(k):
                    mask_files.add(attrs[k])

    def bake_for(name, image_dir):
        """None: plain texture; a tuple: alpha to bake; False: ambiguous, leave as an image."""
        defs = materials.get(name, [])
        needs = []
        for mdir, a in defs:
            factor = float(a.get("colorization-factor", "0") or 0)
            colorize = (a.get("colorization-mask") or factor > 0.0 or
                        a.get("colorizable", "N").upper() in ("Y", "TRUE", "1"))
            if colorize:
                if material_shader(a) in alpha_sh:
                    needs.append(None)
                else:
                    mp = os.path.join(image_dir, a["colorization-mask"]) if a.get("colorization-mask") else None
                    needs.append(("colorize", mp, factor))
            elif a.get("mask"):
                needs.append(("mask", os.path.join(image_dir, a["mask"]), 0.0))
            else:
                needs.append(None)
        if not needs:
            return None
        if any(n != needs[0] for n in needs):
            return False
        n = needs[0]
        if n and n[1] and not os.path.exists(n[1]):
            return False
        return n

    # Textures STK loads through SPTexture: named by meshes or as material layers...
    names = set(layer_names)
    for p in spm_files:
        with open(p, "rb") as f:
            data = f.read()
        for off, n in spm_textures(data):
            names.add(data[off:off + n].decode("latin-1").rsplit("/", 1)[-1])
    # ...minus anything STK also loads as an ordinary image (masks, scene/kart/track/particle
    # XML, GUI files, code).
    other = set(keep) | mask_files
    for p in text_files:
        if os.path.basename(p) == "materials.xml":
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            other.update(re.findall(r"([A-Za-z0-9_.-]+\.(?:png|jpg))", f.read()))
    names -= other
    jobs = []
    skipped = 0
    tmpdir = os.path.join(os.path.dirname(out), "astc-tmp")
    os.makedirs(tmpdir, exist_ok=True)
    for dirpath, _, filenames in os.walk(out):
        if top_dir(os.path.relpath(dirpath, out)) not in TEX_DIRS:
            continue
        for name in filenames:
            if name in names:
                bake = bake_for(name, dirpath)
                if bake is False:
                    skipped += 1
                    continue
                jobs.append((os.path.join(dirpath, name), tool, tmpdir, bake))
    print(f"ASTC {ASTC_BLOCK[0]}x{ASTC_BLOCK[1]}: {len(jobs)} textures "
          f"({sum(1 for j in jobs if j[3])} with a baked mask), {skipped} ambiguous left as images")
    with multiprocessing.Pool() as pool:
        done = pool.map(astc_one, jobs, chunksize=4)
    shutil.rmtree(tmpdir, ignore_errors=True)
    print(f"ASTC: {sum(1 for _, n in done if n)} compressed, {sum(1 for _, n in done if not n)} failed")


def reencode_ogg(job):
    tool, path = job
    music = "/music/" in path
    quality = "0.2" if music else "0.3"
    tmp = path + ".tmp.ogg"
    r = subprocess.run([tool, path, tmp, quality], capture_output=True)
    if r.returncode == 0 and os.path.exists(tmp) and os.path.getsize(tmp) < os.path.getsize(path):
        os.replace(tmp, path)
    elif os.path.exists(tmp):
        os.remove(tmp)


if __name__ == "__main__":
    main()
