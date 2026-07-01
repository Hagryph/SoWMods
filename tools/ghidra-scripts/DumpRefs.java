// DumpRefs.java - targeted xref tracer + RTTI class recovery for the Skyrim UI subsystem.
// Run: analyzeHeadless C:\dev\re\ghidra-proj SkyrimSE -process SkyrimSE.exe -noanalysis
//          -scriptPath <dir> -postScript DumpRefs.java <out.txt>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DumpRefs extends GhidraScript {

    private PrintWriter out;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = (args != null && args.length > 0) ? args[0]
            : "C:\\Users\\Yannis\\Desktop\\Desktop\\projects\\SkyrimMods\\HagUI\\ghidra\\ui_refs.txt";
        File f = new File(outPath);
        if (f.getParentFile() != null) f.getParentFile().mkdirs();
        out = new PrintWriter(new FileWriter(f));

        out.println("# Targeted xref trace + RTTI recovery (imageBase 0x140000000)");
        out.println();

        // ---- A. RTTI class names from .?AV / .?AU type descriptors ----
        out.println("== RTTI class names (UI-relevant) ==");
        Listing listing = currentProgram.getListing();
        DataIterator di = listing.getDefinedData(true);
        List<String> classes = new ArrayList<>();
        String[] clsKw = {"menu", "scaleform", "gfx", "movie", "cursor", "hud", "ui", "interface", "fader"};
        while (di.hasNext() && !monitor.isCancelled()) {
            Data d = di.next();
            Object v;
            try { v = d.getValue(); } catch (Exception e) { continue; }
            if (!(v instanceof String)) continue;
            String s = (String) v;
            if (!s.startsWith(".?A")) continue;            // MSVC type descriptor
            String low = s.toLowerCase();
            for (String k : clsKw) {
                if (low.contains(k)) {
                    classes.add(String.format("0x%s  %s", d.getAddress(), demangle(s)));
                    break;
                }
            }
        }
        Collections.sort(classes);
        for (String c : classes) out.println(c);
        out.println("# " + classes.size() + " UI classes");
        out.println();

        // ---- B. Anchor xref traces (2 hops: string -> pointer -> code) ----
        String[] anchors = {
            "BSScaleformFileOpener failed to open",
            "Interface/Exported/%s.gfx",
            "LoadMovieImageCallback failed",
            "GFxLoader read failed",
            "Journal Menu",
            "InventoryMenu",
            "Cursor Menu",
            "Console",
        };
        for (String a : anchors) traceAnchor(listing, a);

        out.close();
        println("DumpRefs: wrote " + f.getAbsolutePath());
    }

    private void traceAnchor(Listing listing, String anchor) {
        out.println("== anchor: \"" + anchor + "\" ==");
        DataIterator di = listing.getDefinedData(true);
        int hits = 0;
        while (di.hasNext() && hits < 6) {
            Data d = di.next();
            Object v;
            try { v = d.getValue(); } catch (Exception e) { continue; }
            if (!(v instanceof String)) continue;
            String s = (String) v;
            if (!s.contains(anchor)) continue;
            hits++;
            Address strAddr = d.getAddress();
            out.println(String.format("  string @0x%s : \"%s\"", strAddr,
                s.length() > 48 ? s.substring(0, 48) + "..." : s));

            Reference[] refs = getReferencesTo(strAddr);
            if (refs.length == 0) { out.println("    (no direct references)"); continue; }
            for (Reference r : refs) {
                Address from = r.getFromAddress();
                Function fn = getFunctionContaining(from);
                if (fn != null) {
                    out.println(String.format("    <- CODE 0x%s  %s  in FUNC 0x%s",
                        from, r.getReferenceType(), fn.getEntryPoint()));
                } else {
                    out.println(String.format("    <- data 0x%s  %s  [not in func] -> hop:",
                        from, r.getReferenceType()));
                    Reference[] refs2 = getReferencesTo(from);
                    int shown = 0;
                    for (Reference r2 : refs2) {
                        if (shown++ >= 8) break;
                        Address from2 = r2.getFromAddress();
                        Function fn2 = getFunctionContaining(from2);
                        out.println(String.format("        <<- 0x%s  %s  %s",
                            from2, r2.getReferenceType(),
                            fn2 != null ? "FUNC 0x" + fn2.getEntryPoint() : "[data]"));
                    }
                }
            }
        }
        if (hits == 0) out.println("  (string not found as defined data)");
        out.println();
    }

    // ".?AVBSScaleformManager@@" -> "BSScaleformManager"
    private String demangle(String td) {
        String s = td;
        if (s.startsWith(".?AV") || s.startsWith(".?AU")) s = s.substring(4);
        int at = s.indexOf("@@");
        if (at >= 0) s = s.substring(0, at);
        return s;
    }
}
