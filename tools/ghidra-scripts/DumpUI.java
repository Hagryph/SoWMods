// DumpUI.java - Ghidra headless post-script.
// Mines SkyrimSE.exe's Scaleform/UI subsystem so we can hook it by hand (no Address Library).
// Run after analysis:
//   analyzeHeadless <proj> SkyrimSE -process SkyrimSE.exe -noanalysis \
//       -scriptPath <this dir> -postScript DumpUI.java "<output .txt path>"
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DumpUI extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = (args != null && args.length > 0) ? args[0]
            : "C:\\Users\\Yannis\\Desktop\\Desktop\\projects\\SkyrimMods\\HagUI\\ghidra\\ui_dump.txt";
        File outFile = new File(outPath);
        if (outFile.getParentFile() != null) outFile.getParentFile().mkdirs();
        PrintWriter out = new PrintWriter(new FileWriter(outFile));

        Listing listing = currentProgram.getListing();
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        FunctionManager funcMgr = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        long imageBase = currentProgram.getImageBase().getOffset();

        out.println("# Skyrim SE UI reverse-engineering dump");
        out.println("# program : " + currentProgram.getName());
        out.println("# compiler: " + currentProgram.getCompiler());
        out.println("# imageBase = 0x" + Long.toHexString(imageBase)
            + "   (runtime RVA = fileAddr - imageBase)");
        out.println();

        // ---- 1. UI-related defined strings + the functions that reference them ----
        String[] keywords = {"scaleform", "gfx", ".swf", "loadmovie", "movieview",
                             "interface/", "menu", "cursor", "gfxvalue", "invoke"};
        Map<Function, Set<String>> menuRefs = new HashMap<>();   // registrar heuristic
        int strCount = 0;

        out.println("== UI strings and referencing functions ==");
        DataIterator dataIt = listing.getDefinedData(true);
        while (dataIt.hasNext() && !monitor.isCancelled()) {
            Data d = dataIt.next();
            String s;
            try {
                Object val = d.getValue();
                if (!(val instanceof String)) continue;
                s = (String) val;
            } catch (Exception ex) { continue; }

            String low = s.toLowerCase();
            boolean isMenu = low.contains("menu");
            boolean hit = isMenu;
            if (!hit) for (String k : keywords) if (low.contains(k)) { hit = true; break; }
            if (!hit) continue;
            strCount++;

            StringBuilder fns = new StringBuilder();
            ReferenceIterator it = refMgr.getReferencesTo(d.getAddress());
            while (it.hasNext()) {
                Address from = it.next().getFromAddress();
                Function f = funcMgr.getFunctionContaining(from);
                if (f != null) {
                    fns.append(" ").append(f.getEntryPoint());
                    if (isMenu) menuRefs.computeIfAbsent(f, x -> new HashSet<>()).add(s);
                }
            }
            out.println(String.format("0x%s  \"%s\"  <-%s",
                d.getAddress(), s.replace("\n", "\\n"), fns));
        }
        out.println("# total UI strings: " + strCount);
        out.println();

        // ---- 2. Menu-registration candidates ----
        out.println("== Menu-registrar candidates (functions referencing the most distinct *Menu* strings) ==");
        List<Map.Entry<Function, Set<String>>> ranked = new ArrayList<>(menuRefs.entrySet());
        ranked.sort((a, b) -> b.getValue().size() - a.getValue().size());
        for (int i = 0; i < ranked.size() && i < 20; i++) {
            Map.Entry<Function, Set<String>> e = ranked.get(i);
            out.println(String.format("%-44s entry=0x%s  distinctMenus=%d",
                e.getKey().getName(), e.getKey().getEntryPoint(), e.getValue().size()));
        }
        out.println();
        if (!ranked.isEmpty()) {
            Map.Entry<Function, Set<String>> top = ranked.get(0);
            out.println("== Top registrar candidate: " + top.getKey().getName()
                + " @0x" + top.getKey().getEntryPoint() + " -> menus registered ==");
            List<String> names = new ArrayList<>(top.getValue());
            Collections.sort(names);
            for (String n : names) out.println("   " + n);
            out.println();
        }

        // ---- 3. Recovered C++ classes (RTTI) matching UI keywords ----
        out.println("== Recovered classes (RTTI) matching UI keywords ==");
        String[] classKw = {"scaleform", "gfx", "menu", "movie", "cursor", "hud", "interface", "bsui", "uimessage"};
        Iterator<GhidraClass> classes = st.getClassNamespaces();
        while (classes.hasNext()) {
            GhidraClass gc = classes.next();
            String n = gc.getName().toLowerCase();
            for (String k : classKw) {
                if (n.contains(k)) {
                    Symbol sym = gc.getSymbol();
                    out.println(String.format("%-52s @0x%s", gc.getName(true),
                        sym != null ? sym.getAddress().toString() : "?"));
                    break;
                }
            }
        }
        out.println();

        // ---- 4. Named symbols for the functions we want to hook/call ----
        out.println("== Symbols matching key UI function names ==");
        String[] symKw = {"LoadMovie", "Register", "GFxValue", "Invoke",
                          "ProcessMessage", "ShowMenu", "OpenMenu",
                          "ScaleformManager", "UIMessage", "MenuManager"};
        for (String k : symKw) {
            SymbolIterator it = st.getSymbolIterator("*" + k + "*", true);
            int c = 0;
            while (it.hasNext() && c < 30) {
                Symbol sym = it.next();
                out.println(String.format("%-62s @0x%s", sym.getName(true), sym.getAddress()));
                c++;
            }
        }

        out.close();
        println("DumpUI: wrote " + outFile.getAbsolutePath());
    }
}
