# Bakes the two 3D models of the C64 for the Info view into gui/c64_data.h (needs numpy and Pillow).
# usage: python3 tools/bake_c64.py MODEL_6581.glb MODEL_8580.glb > gui/c64_data.h
#   MODEL_6581.glb: the C64 with the MOS 6581R4 (the clear case), MODEL_8580.glb: the C64 with the CSG 8580R5
# Each model becomes four meshes, drawn in this order (gui/c64.inc):
#   0 solid  - every opaque part with a plain colour (a colour per vertex, flat shading in the shader)
#   1 keys   - the keyboard, textured with a photo (RGB565)
#   2 badge  - the badge, textured (RGB565)
#   3 glass  - the see-through parts (case, plate): a colour with alpha per vertex, blended last
#   4 led    - the LEDs (nodes PowerLED, GreenLED, LED0..3): drawn bright, their colour follows the light of the Info view
# Positions are int16 in 1/20000 of half the length of the case (the model is 0.4 long: x in -1..1).
import io, os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glb import load, acc

Q = 20000.0
HALF = 0.2
# the keyboard of the 8580R5 (C64C) model sits too far back: the part with the vents (back) has to be about twice as long, so the keyboard, its plate, the badge and the LEDs move to the front
SHIFT_Z = {'C64_8580': 0.045}
MOVES = ('Keys_body', 'Keys_top', 'FKeyPlate', 'Badge', 'LED0', 'LED1', 'LED2', 'LED3')
SKIP = ('KeyPlate',)                    # the black plate under the keys: the real case has none
TEX = ((1024, 256), (512, 64))         # the keys picture (2048 x 512) and the badge (about 800 x 110), scaled


def arr(name, ctype, vals, per=24):
    s = 'static const %s %s[%d] = {\n' % (ctype, name, len(vals))
    line = []
    for v in vals:
        line.append(str(int(v)))
        if len(line) == per:
            s += ','.join(line) + ',\n'
            line = []
    if line:
        s += ','.join(line) + ',\n'
    return s + '};\n'


def rgb565(img):
    a = np.asarray(img.convert('RGB'), dtype=np.uint16)
    return ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)


def bake(path, tag):
    j, b = load(path)
    parts = {k: ([], [], [], []) for k in ('solid', 'keys', 'badge', 'glass', 'led')}      # positions, uvs, colours, indices
    base = {k: 0 for k in parts}
    ledpts = []                                                      # the centres of the LED parts (model units): light points for the glow
    fk = [acc(j, b, j['meshes'][n['mesh']]['primitives'][0]['attributes']['POSITION'])[0] for n in j['nodes'] if n.get('name') == 'FKeyPlate']
    FK_X = float(fk[0][:, 0].min()) - 0.003 if fk else 9.0          # keys right of this x are the four function keys (red)
    for n in j['nodes']:
        if n.get('name') in SKIP: continue
        pr = j['meshes'][n['mesh']]['primitives'][0]
        p, _ = acc(j, b, pr['attributes']['POSITION'])
        if n.get('name') in MOVES and SHIFT_Z.get(tag):
            p = p.copy(); p[:, 2] += SHIFT_Z[tag]
        idx, _ = acc(j, b, pr['indices'])
        mat = j['materials'][pr['material']]
        pb = mat['pbrMetallicRoughness']
        if 'baseColorTexture' in pb:
            kind = 'keys' if pb['baseColorTexture']['index'] == 0 else 'badge'
            uv, _ = acc(j, b, pr['attributes']['TEXCOORD_0'])
            col = [255, 255, 255, 255]
        else:
            f = pb.get('baseColorFactor', [1, 1, 1, 1])
            col = [int(round(x * 255)) for x in f]
            kind = 'glass' if mat.get('alphaMode') == 'BLEND' else 'solid'
            if n.get('name', '') in ('PowerLED', 'GreenLED') or n.get('name', '').startswith('LED'): kind = 'led'
            if kind == 'glass': col[3] = 128                      # every case is equally milky (alpha 0.5)
            uv = np.zeros((len(p), 2), dtype=np.float32)
        if kind == 'led': ledpts.append(p.astype(np.float64).mean(axis=0))
        P, U, C, I = parts[kind]
        cols = np.tile(np.array(col, dtype=np.uint8), (len(p), 1))
        if n.get('name') in ('Keys_top', 'Keys_body'):                # 3D keys without a picture: dark caps, the function keys red
            cols[:] = (24, 24, 28, 255)
            cols[p[:, 0] >= FK_X] = (196, 28, 24, 255)
        P.append(p.astype(np.float64)); U.append(uv.astype(np.float64)); C.append(cols)
        I.append(idx.astype(np.int64) + base[kind]); base[kind] += len(p)
    # the model lies flat, x along the case: normalise so that x is -1..1 and the middle is at the origin
    allp = np.vstack([np.vstack(parts[k][0]) for k in parts if parts[k][0]])
    mn, mx = allp.min(axis=0), allp.max(axis=0)
    centre = np.array([(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2])
    out = ''
    sizes = []
    for k in ('solid', 'keys', 'badge', 'glass', 'led'):
        P, U, C, I = parts[k]
        if not P:
            out += '#define %s_%s_NV 0\n#define %s_%s_NI 0\n' % (tag, k.upper(), tag, k.upper())
            sizes.append((0, 0)); continue
        p = (np.vstack(P) - centre) / HALF
        pos = np.clip(np.round(p * Q), -32767, 32767).astype(np.int16)
        uvs = np.clip(np.round(np.vstack(U) * 65535), 0, 65535).astype(np.uint16)
        col = np.vstack(C)
        idx = np.concatenate(I).astype(np.uint16)
        out += arr('%s_%s_pos' % (tag.lower(), k), 'int16_t', pos.flatten())
        out += arr('%s_%s_uv' % (tag.lower(), k), 'uint16_t', uvs.flatten())
        out += arr('%s_%s_col' % (tag.lower(), k), 'uint8_t', col.flatten())
        out += arr('%s_%s_idx' % (tag.lower(), k), 'uint16_t', idx)
        out += '#define %s_%s_NV %d\n#define %s_%s_NI %d\n' % (tag, k.upper(), len(pos), tag, k.upper(), len(idx))
        sizes.append((len(pos), len(idx)))
    lp = [((q - centre) / HALF) for q in ledpts][:6]
    while len(lp) < 6: lp.append(np.array([0.0, -9.0, 0.0]))        # unused points far away
    out += arr('%s_ledpts' % tag.lower(), 'int16_t', np.clip(np.round(np.array(lp) * Q), -32767, 32767).astype(np.int16).flatten())
    for i, name in enumerate(('KEYS', 'BADGE')):
        im = j['images'][i]; bv = j['bufferViews'][im['bufferView']]
        img = Image.open(io.BytesIO(b[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']])).convert('RGB')
        w, h = TEX[i]
        img = img.resize((w, h), Image.LANCZOS)
        out += arr('%s_%s_tex' % (tag.lower(), name.lower()), 'uint16_t', rgb565(img).flatten(), 32)
        out += '#define %s_%s_TW %d\n#define %s_%s_TH %d\n' % (tag, name, w, tag, name, h)
    ext = (mx - mn) / HALF
    out += '#define %s_SIZE_X %.4ff\n#define %s_SIZE_Y %.4ff\n#define %s_SIZE_Z %.4ff\n' % (tag, ext[0], tag, ext[1], tag, ext[2])
    sys.stderr.write('%s: %s\n' % (tag, sizes))
    return out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__); sys.exit(2)
    print('/* gui/c64_data.h - the C64 for the Info view: 6581 = the clear case with the MOS 6581R4, 8580 = the CSG 8580R5 one. Made by tools/bake_c64.py from the 3D models. */')
    print('#pragma once\n#include <stdint.h>')
    print(bake(sys.argv[1], 'C64_6581'))
    print(bake(sys.argv[2], 'C64_8580'))
