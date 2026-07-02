#!/usr/bin/env python3
# extract_sow_items.py - pull the item/gear catalog out of the extracted game.gamedb (GADB).
#
# Pipeline to get game.gamedb (once):
#   QuickBMS: quickbms_4gb_files.exe -o -f "*game.gamedb" shadow_of_mordor.bms Patch_02.arch06 <out>
#   (QuickBMS Oodle-decompresses the ~888 chunks -> database\game\game.gamedb, ~55 MB)
#
# game.gamedb format: magic 'GADB', a header of section offsets, then inline NUL-separated
# type names + record names + binary field data. We don't need the full record parser to build
# the catalog: the record NAMES are ASCII and follow strict conventions, so we scan for them.
#
#   python extract_sow_items.py <game.gamedb> [outdir]
import sys, os, re, json, collections

def ascii_strings(data, minlen=3):
    # yield (offset, string) for runs of printable ASCII
    out, start, cur = [], None, []
    for i, b in enumerate(data):
        if 0x20 <= b < 0x7f:
            if start is None: start = i
            cur.append(b)
        else:
            if start is not None and len(cur) >= minlen:
                out.append((start, bytes(cur).decode('ascii')))
            start, cur = None, []
    if start is not None and len(cur) >= minlen:
        out.append((start, bytes(cur).decode('ascii')))
    return out

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\dev\quickbms\extract\database\game\game.gamedb'
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__)) + r'\sow_catalog'
    os.makedirs(outdir, exist_ok=True)
    data = open(path, 'rb').read()
    assert data[:4] == b'GADB', 'not a GADB file'

    def find(pat):  # byte-regex substring scan (like grep -ao), returns sorted unique strs
        return sorted({m.decode('ascii') for m in re.findall(pat, data)})

    # Gear pieces: GearArt_<Type>_<Theme>_<Tier>  (the equippable gear templates)
    gear = find(rb'GearArt_[A-Za-z0-9_]+')
    parsed = []
    for g in gear:
        parts = g[len('GearArt_'):].split('_')
        wtype = parts[0] if len(parts) > 0 else ''
        theme = parts[1] if len(parts) > 1 else ''
        tier  = parts[2] if len(parts) > 2 else ''
        parsed.append({'record': g, 'weaponType': wtype, 'theme': theme, 'tier': tier})

    # Other item-category records by leading keyword (byte-regex scan)
    cats = collections.OrderedDict()
    for kw in ('Rune', 'Runegem', 'Gem', 'Weapon', 'Armor', 'GearTag'):
        cats[kw] = find(('\\b' + kw + r'[A-Za-z0-9_]{2,}').encode())

    # write catalog
    with open(outdir + r'\gear.json', 'w') as f:
        json.dump(parsed, f, indent=1)
    with open(outdir + r'\gear.txt', 'w') as f:
        for g in gear: f.write(g + '\n')
    for kw, lst in cats.items():
        with open(outdir + '\\' + kw.lower() + '.txt', 'w') as f:
            for s in lst: f.write(s + '\n')

    # summary
    by_type = collections.Counter(p['weaponType'] for p in parsed)
    by_theme = collections.Counter(p['theme'] for p in parsed if p['theme'])
    print(f'game.gamedb: {len(data):,} bytes')
    print(f'GEAR pieces (GearArt_): {len(gear)}')
    print('  by weapon/gear type:', dict(by_type.most_common(12)))
    print('  by theme:', dict(by_theme.most_common(15)))
    for kw, lst in cats.items():
        print(f'{kw}: {len(lst)} records')
    print(f'\nwrote catalog to {outdir}')

if __name__ == '__main__':
    main()
