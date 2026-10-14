"""Build the README images in ../docs: transparent sprite thumbnails and a room preview."""
from pathlib import Path
from PIL import Image
import convert_sprites as c

ROOT = c.ROOT
DOCS = ROOT / "docs"


def thumb(src, dst):
    im, a = c.key_dog(src)
    box = a.getbbox()
    rgba = im.crop(box).convert("RGBA")
    rgba.putalpha(a.crop(box))
    rgba.save(dst)


def main():
    for folder, out in [("sprites_src", "sprites"), ("sprites_src_v1_orange", "sprites_v1_orange")]:
        (DOCS / out).mkdir(parents=True, exist_ok=True)
        for p in sorted((ROOT / folder).glob("*.png")):
            if p.stem != "room":
                thumb(p, DOCS / out / p.name)

    room = Image.open(ROOT / "sprites_src" / "room.png").convert("RGB")
    room.resize((584, 440), Image.LANCZOS).save(DOCS / "room.png")

    # hero preview: a few poses standing in the room, at 2x the on-device size
    s = max(c.SCENE_W / room.width, c.SCENE_H / room.height)
    room = room.resize((round(room.width * s), round(room.height * s)), Image.LANCZOS)
    left = (room.width - c.SCENE_W) // 2
    room = room.crop((left, 0, left + c.SCENE_W, c.SCENE_H)).resize((736, 560), Image.LANCZOS)
    for x, name in [(20, "idle"), (245, "walk1"), (500, "happy")]:
        im = Image.open(DOCS / "sprites" / f"{name}.png")
        im = im.resize((int(im.width * 1.6), int(im.height * 1.6)), Image.LANCZOS)
        room.paste(im, (x, 520 - im.height), im)
    room.save(DOCS / "preview.png")
    print("docs written")


if __name__ == "__main__":
    main()
