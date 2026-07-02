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

# ---- sophisticated grouping: gear -> slot -> set -> tier, everything sorted deterministically ----

# weapon-type token -> (slot sort key, slot display name). Faction/tint gear folds into a slot.
def slot_of(wtype):
    w = wtype
    def has(*xs): return any(x in w for x in xs)
    if has('Sword'):  return (1, 'Sword')
    if has('Dagger'): return (2, 'Dagger')
    if has('Bow'):    return (3, 'Bow')
    if has('Armor'):  return (4, 'Armor')
    if has('Cape', 'Cloak'): return (5, 'Cloak')
    if has('Talion'): return (6, 'Talion')
    return (9, wtype)

# set / theme -> (sort key, display). Rarity buckets first, then named legendary sets, then skins.
RARITY = {'1Common': 0, '2Uncommon': 1, '3Rare': 2, '4Epic': 3, '5Legendary': 4}
LEGEND_SETS = ['Celebrimbor', 'Vendetta', 'Dark', 'Feral', 'Machine', 'Marauder', 'Mystic',
               'Outlaw', 'Ringwraith', 'Slaughter', 'Terror', 'Warmonger', 'Ringcraft']
def set_of(theme, wtype):
    if theme in RARITY: return (0, RARITY[theme], theme.lstrip('012345'))
    if theme in LEGEND_SETS: return (1, LEGEND_SETS.index(theme), theme)
    # faction gear carries its faction in the weaponType token (RohanArmor, GondorianCloak, ...)
    for fac in ('Rohan', 'Ranger', 'Numenorean', 'Gondorian'):
        if wtype.startswith(fac): return (2, 0, fac)
    if theme.startswith('Tint'): return (3, int(theme[4:] or 0), 'Skins')
    return (4, 0, theme or 'Other')   # keep the item's own theme name; group all same-theme items together

def tier_key(tier):
    return int(tier[1:]) if tier.startswith('T') and tier[1:].isdigit() else 99

# ---- display name: turn an internal record token into a human-readable label ----
# Records are strictly conventional (GearArt_<Slot>_<Theme>_<Tier>, Weapons_<Name>_<Variant>, Rune...),
# so we synthesise a clean label offline: drop the category prefix, strip rarity digits, expand the Tint
# skins, and split CamelCase into words. These are DERIVED labels, not the game's localised strings -
# the loc table would be a separate extraction if exact in-game wording is ever needed.
_PREFIXES = ('GearArt', 'Gear', 'Weapons', 'Weapon', 'Runegem', 'Rune', 'Gem', 'Armor')
def humanize(rec):
    toks = rec.split('_')
    if len(toks) > 1 and toks[0] in _PREFIXES:
        toks = toks[1:]                                    # drop the leading category prefix
    out = []
    for t in toks:
        t = re.sub(r'^\d+', '', t)                         # 5Legendary -> Legendary, 3Rare -> Rare
        if t.startswith('Tint'):
            t = ('Skin ' + t[4:]).strip()                  # Tint05 -> "Skin 05"
        t = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', ' ', t)      # camelCase -> "camel Case"
        if t:
            out.append(t)
    return ' '.join(out).strip() or rec

# ---- facets: the independently-filterable dimensions of an item ----
_FACTIONS = ('Rohan', 'Ranger', 'Numenorean', 'Gondorian')
def gear_facets(wtype, theme):
    slot = slot_of(wtype)[1]
    rarity = setname = ''
    if theme in RARITY:                 # 3Rare -> Rare, 5Legendary -> Legendary, ...
        rarity = theme.lstrip('0123456789')
    elif theme in LEGEND_SETS:          # named legendary set (all Legendary rarity)
        rarity, setname = 'Legendary', theme
    elif theme.startswith('Tint'):      # cosmetic skin
        setname = 'Skins'
    if not setname:                     # faction gear carries its faction in the type token
        for fac in _FACTIONS:
            if wtype.startswith(fac): setname = fac; break
    return slot, setname, rarity

# records that are camera rigs / menu states / render modes / build scaffolding, not real items
_JUNK = ('FreeCam', 'SubMenu', 'Select', 'Moment', 'Mode', 'Transform', 'Step', 'Camera', 'Menu',
         'Debug', 'Test', 'Combine', 'Tutorial', 'Preview', 'Placeholder', 'Dummy', 'Unused',
         'Material', 'Default', 'Template', 'Anim', 'Setup', 'Base', 'Root')
def is_item(rec):
    return not any(j in rec for j in _JUNK)

def write_mod_catalog(parsed, cats):
    import os
    root = os.path.dirname(os.path.abspath(__file__))
    mod = os.path.join(root, '..', 'mods', 'InventoryEditor')
    os.makedirs(mod, exist_ok=True)

    # Each row: (sortkey, category, slot, set, rarity, tier, record, display). category is the group
    # header, slot the sub-group; set/rarity/tier are the extra filter facets. '-' = facet absent.
    rows = []
    for p in parsed:
        so, _slot = slot_of(p['weaponType'])
        s0, s1, setbucket = set_of(p['theme'], p['weaponType'])
        slot, setname, rarity = gear_facets(p['weaponType'], p['theme'])
        rows.append(((0, so, s0, s1, setbucket, tier_key(p['tier']), p['record']),
                     'Gear', slot or '-', setname or '-', rarity or '-', p['tier'] or '-',
                     p['record'], humanize(p['record'])))
    catmap = [('Rune', cats['Rune']), ('Runegem', cats['Runegem']), ('Gem', cats['Gem']),
              ('Weapon', cats['Weapon']), ('Armor', cats['Armor'])]
    dropped = 0
    for cat, lst in catmap:
        for rec in lst:
            if not is_item(rec): dropped += 1; continue
            rows.append(((1, cat, rec), cat, '-', '-', '-', '-', rec, humanize(rec)))
    rows.sort(key=lambda r: r[0])

    # mod-consumable TSV (loaded at runtime): category slot set rarity tier record display
    catalog = os.path.join(mod, 'InventoryEditor.catalog')
    with open(catalog, 'w', encoding='utf-8') as f:
        f.write('# SoW item catalog (auto-generated by tools/extract_sow_items.py from game.gamedb)\n')
        f.write('# category\tslot\tset\trarity\ttier\trecord\tdisplay\n')
        for r in rows:
            f.write('\t'.join([r[1], r[2], r[3], r[4], r[5], r[6], r[7]]) + '\n')

    # human-readable grouped view (category -> slot)
    with open(os.path.join(root, 'sow_catalog', 'catalog.md'), 'w', encoding='utf-8') as f:
        f.write('# Shadow of War — item catalog\n\n')
        cur_cat = cur_slot = None
        for r in rows:
            _, cat, slot, sset, rar, tier, rec, disp = r
            if cat != cur_cat: f.write(f'\n## {cat}\n'); cur_cat, cur_slot = cat, None
            if slot != '-' and slot != cur_slot: f.write(f'\n### {slot}\n'); cur_slot = slot
            extra = '  ·  '.join(x for x in (sset, rar, tier) if x not in ('-', ''))
            f.write(f'- {disp}  `{rec}`' + (f'  ({extra})' if extra else '') + '\n')
    print(f'grouped catalog -> {catalog} ({len(rows)} records, {dropped} junk dropped)')


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

    # ---- sophisticated grouping + sorting, emitted as the InventoryEditor mod's catalog ----
    write_mod_catalog(parsed, cats)

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
