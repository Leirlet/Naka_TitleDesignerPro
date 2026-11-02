# python sheet.py <tdp> <out.png> <frames comma separated> [width]
import os, subprocess, sys
from PIL import Image, ImageDraw

tdp, out, frames = sys.argv[1], sys.argv[2], [int(f) for f in sys.argv[3].split(',')]
width = int(sys.argv[4]) if len(sys.argv) > 4 else 640
exe = r'E:\_Title_Designer_Pro\build\bin\Release\tdp_render_test.exe'
tmpdir = os.path.dirname(out)
tiles = []
for f in frames:
    bmp = os.path.join(tmpdir, f'_f{f}.bmp')
    subprocess.run([exe, tdp, bmp, str(f), str(width)], check=True, capture_output=True)
    im = Image.open(bmp).convert('RGB')
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 70, 22], fill=(0, 0, 0))
    d.text((4, 4), f'f{f}', fill=(255, 255, 0))
    tiles.append(im)
cols = 3
w, h = tiles[0].size
sheet = Image.new('RGB', (w * cols, h * ((len(tiles) + cols - 1) // cols)), (40, 40, 40))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
print('saved', out, sheet.size)
