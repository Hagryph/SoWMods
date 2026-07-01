// BuildXref.java - recompute code->string references from instruction bytes,
// bypassing Ghidra's (incomplete) data-reference DB. Decodes RIP-relative LEA
// (REX 8D /r  modrm mod=00 rm=101  disp32) and resolves the target.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class BuildXref extends GhidraScript {

    private String trim(String s) {
        s = s.replace("\n", "\\n").replace("\r", "");
        return s.length() > 44 ? s.substring(0, 44) + "..." : s;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = (args != null && args.length > 0) ? args[0]
            : "C:\\Users\\Yannis\\Desktop\\Desktop\\projects\\SkyrimMods\\HagUI\\ghidra\\ui_xref.txt";
        File f = new File(outPath);
        if (f.getParentFile() != null) f.getParentFile().mkdirs();
        PrintWriter out = new PrintWriter(new FileWriter(outPath));

        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        // 1. collect target string addresses (menu names + loader/scaleform strings)
        String[] loaderKw = {"scaleform", "gfxloader", "loadmovie", "movieview",
                             "interface/exported", "gfxvalue", "gfxsprite",
                             "externalinterface", "gfxfunctionhandler", ".gfx", ".swf"};
        Map<Long, String> targets = new HashMap<>();
        DataIterator di = listing.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v;
            try { v = d.getValue(); } catch (Exception e) { continue; }
            if (!(v instanceof String)) continue;
            String s = (String) v;
            String low = s.toLowerCase();
            boolean hit = low.contains("menu");
            if (!hit) for (String k : loaderKw) if (low.contains(k)) { hit = true; break; }
            if (hit) targets.put(d.getAddress().getOffset(), s);
        }
        out.println("# string targets: " + targets.size());

        // 2. scan every LEA, decode RIP-relative target, match against targets
        Map<Long, Set<Address>> refMap = new HashMap<>();
        long scanned = 0, leas = 0, hits = 0;
        InstructionIterator ii = listing.getInstructions(true);
        while (ii.hasNext() && !monitor.isCancelled()) {
            Instruction ins = ii.next();
            scanned++;
            if (!"LEA".equals(ins.getMnemonicString())) continue;
            leas++;
            int len = ins.getLength();
            if (len < 7) continue;
            byte[] b;
            try { b = ins.getBytes(); } catch (Exception e) { continue; }
            int idx = 0;
            while (idx < len && (((b[idx] & 0xF0) == 0x40) || (b[idx] & 0xFF) == 0x66 || (b[idx] & 0xFF) == 0x67)) idx++;
            if (idx >= len || (b[idx] & 0xFF) != 0x8D) continue;
            if (idx + 1 >= len) continue;
            int modrm = b[idx + 1] & 0xFF;
            if ((modrm & 0xC7) != 0x05) continue;  // need mod=00, rm=101 (RIP-relative)
            int disp = (b[len - 4] & 0xFF) | ((b[len - 3] & 0xFF) << 8)
                     | ((b[len - 2] & 0xFF) << 16) | ((b[len - 1] & 0xFF) << 24);
            long target = ins.getAddress().getOffset() + len + disp;
            if (!targets.containsKey(target)) continue;
            hits++;
            Function fn = fm.getFunctionContaining(ins.getAddress());
            if (fn != null) refMap.computeIfAbsent(target, x -> new HashSet<>()).add(fn.getEntryPoint());
        }
        out.println("# scanned " + scanned + " instrs, " + leas + " LEAs, " + hits + " hits");
        out.println();

        // A. loader/scaleform strings -> the functions that use them (direct hits)
        out.println("== loader/Scaleform strings -> referencing functions ==");
        List<Long> keys = new ArrayList<>(targets.keySet());
        Collections.sort(keys);
        for (Long k : keys) {
            String s = targets.get(k);
            String low = s.toLowerCase();
            boolean loader = false;
            for (String kw : loaderKw) if (low.contains(kw)) { loader = true; break; }
            if (!loader) continue;
            Set<Address> fns = refMap.get(k);
            if (fns == null || fns.isEmpty()) continue;
            StringBuilder sb = new StringBuilder();
            for (Address a : fns) sb.append(" 0x").append(a);
            out.println(String.format("0x%x \"%s\" <-%s", k, trim(s), sb));
        }
        out.println();

        // B. registrar heuristic: function -> distinct *menu* strings it references
        Map<Address, Set<String>> funcMenus = new HashMap<>();
        for (Map.Entry<Long, Set<Address>> e : refMap.entrySet()) {
            String s = targets.get(e.getKey());
            if (s == null || !s.toLowerCase().contains("menu")) continue;
            for (Address a : e.getValue()) funcMenus.computeIfAbsent(a, x -> new HashSet<>()).add(s);
        }
        out.println("== top menu-referencing functions (registrar / ctor candidates) ==");
        List<Map.Entry<Address, Set<String>>> rk = new ArrayList<>(funcMenus.entrySet());
        rk.sort((a, b) -> b.getValue().size() - a.getValue().size());
        for (int i = 0; i < rk.size() && i < 25; i++) {
            out.println(String.format("FUNC 0x%s  distinctMenuStrings=%d",
                rk.get(i).getKey(), rk.get(i).getValue().size()));
        }
        if (!rk.isEmpty()) {
            out.println("\n== menus referenced by top candidate (FUNC 0x" + rk.get(0).getKey() + ") ==");
            List<String> ms = new ArrayList<>(rk.get(0).getValue());
            Collections.sort(ms);
            for (String m : ms) out.println("  " + trim(m));
        }

        out.close();
        println("BuildXref: wrote " + outPath);
    }
}
