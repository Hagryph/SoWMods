#!/usr/bin/env python3
"""Emit a minimal, uncompressed AS2 SWF for the loose-file render proof.
A 512x512 stage: magenta background + a large green filled rectangle. No ActionScript.
If this renders in-game, we see magenta+green where the movie is drawn -> unmistakable proof
that the engine loaded and rendered our loose .swf.
"""
import struct, sys

TWIP = 20  # 1 px = 20 twips

class Bits:
    def __init__(self): self.cur = 0; self.n = 0; self.out = bytearray()
    def write(self, val, nbits):
        for i in range(nbits - 1, -1, -1):
            self.cur = (self.cur << 1) | ((val >> i) & 1); self.n += 1
            if self.n == 8: self.out.append(self.cur); self.cur = 0; self.n = 0
    def flush(self):
        if self.n: self.out.append(self.cur << (8 - self.n)); self.cur = 0; self.n = 0
        return bytes(self.out)

def sbits_needed(vals):
    m = 1
    for v in vals:
        b = v.bit_length() + 1 if v >= 0 else (~v).bit_length() + 1
        m = max(m, b)
    return m

def rect(xmin, ymin, xmax, ymax):
    b = Bits()
    nb = 0
    for v in (xmin, ymin, xmax, ymax): nb = max(nb, v.bit_length())
    nb = max(nb, 1)
    b.write(nb, 5)
    for v in (xmin, ymin, xmax, ymax): b.write(v, nb)
    return b.flush()

def tag(code, data):
    if len(data) < 0x3f:
        return struct.pack('<H', (code << 6) | len(data)) + data
    return struct.pack('<H', (code << 6) | 0x3f) + struct.pack('<I', len(data)) + data

def set_bg(r, g, b):
    return tag(9, bytes((r, g, b)))

def define_shape_rect(shape_id, w, h, rgb):
    # bounds
    data = struct.pack('<H', shape_id) + rect(0, 0, w, h)
    # SHAPEWITHSTYLE: 1 solid fill, 0 lines
    body = bytearray()
    body.append(1)                      # FillStyleCount
    body.append(0x00)                   # fill type: solid
    body += bytes(rgb)                  # RGB (no alpha in DefineShape tag 2)
    body.append(0)                      # LineStyleCount
    data += bytes(body)
    # NumFillBits=1, NumLineBits=0
    b = Bits()
    b.write(1, 4); b.write(0, 4)
    # StyleChangeRecord: non-edge, moveTo (0,0), set fillStyle1=1
    # record flags (5 bits): [reserved0, stateNewStyles0, stateLineStyle0, stateFillStyle1=1, stateFillStyle0=0, stateMoveTo=1]
    # layout: TypeFlag(1)=0 then 5 state bits: StateNewStyles,StateLineStyle,StateFillStyle1,StateFillStyle0,StateMoveTo
    b.write(0, 1)                       # TypeFlag = 0 (non-edge / style change)
    b.write(0, 1)                       # StateNewStyles
    b.write(0, 1)                       # StateLineStyle
    b.write(1, 1)                       # StateFillStyle1
    b.write(0, 1)                       # StateFillStyle0
    b.write(1, 1)                       # StateMoveTo
    # MoveTo (0,0)
    mb = 1
    b.write(mb, 5); b.write(0, mb); b.write(0, mb)
    # FillStyle1 index = 1 (NumFillBits=1)
    b.write(1, 1)
    # Now draw rectangle edges with StraightEdgeRecords.
    # Edges: right(+w,0), down(0,+h), left(-w,0), up(0,-h)
    edges = [(w, 0), (0, h), (-w, 0), (0, -h)]
    nb = sbits_needed([c for e in edges for c in e])
    for (dx, dy) in edges:
        b.write(1, 1)                   # TypeFlag = 1 (edge)
        b.write(1, 1)                   # StraightFlag = 1
        b.write(nb - 2, 4)              # NumBits (stored as NumBits-2)
        b.write(1, 1)                   # GeneralLineFlag = 1 (both dx,dy)
        # dx, dy as signed nb-bit
        b.write(dx & ((1 << nb) - 1), nb)
        b.write(dy & ((1 << nb) - 1), nb)
    # EndShapeRecord: TypeFlag=0 + 5 zero bits
    b.write(0, 6)
    data += b.flush()
    return tag(2, data)

def place_object2(char_id, depth):
    # flags: PlaceFlagHasCharacter(0x02) + PlaceFlagHasMatrix(0x04)
    flags = 0x02 | 0x04
    data = bytearray()
    data.append(flags)
    data += struct.pack('<H', depth)
    data += struct.pack('<H', char_id)
    # identity matrix: HasScale=0, HasRotate=0, TranslateBits=0
    mb = Bits(); mb.write(0, 1); mb.write(0, 1); mb.write(0, 5)
    data += mb.flush()
    return tag(26, bytes(data))

def build():
    W, H = 1280 * TWIP, 720 * TWIP
    body = bytearray()
    body += set_bg(0xFF, 0x00, 0xFF)                              # magenta stage (ignored if loaded)
    # FULL-stage green fill (content, not bg) + a magenta half, so it's unmistakable at any mapping.
    body += define_shape_rect(1, 1280 * TWIP, 720 * TWIP, (0x20, 0xF0, 0x40))  # id 1: full-stage green
    body += define_shape_rect(2, 640 * TWIP, 720 * TWIP, (0xF0, 0x10, 0xC0))   # id 2: left-half magenta
    body += place_object2(1, 1)                                   # green fills stage
    body += place_object2(2, 2)                                   # magenta over left half
    body += tag(1, b'')                                           # ShowFrame
    body += tag(0, b'')                                           # End
    header_after_sig = rect(0, 0, W, H) + struct.pack('<H', 24 * 256) + struct.pack('<H', 1)  # 24fps, 1 frame
    pre = b'FWS' + bytes([6])
    filelen = len(pre) + 4 + len(header_after_sig) + len(body)
    return pre + struct.pack('<I', filelen) + header_after_sig + bytes(body)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'HagUI.swf'
    data = build()
    with open(out, 'wb') as f: f.write(data)
    print(f'wrote {out}: {len(data)} bytes, sig={data[:3]} ver={data[3]}')
