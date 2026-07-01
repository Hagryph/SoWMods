// Trace.java - flexible backward/forward tracer.
//   args: <outfile> <mode: decomp|callers|both> <target> [<target> ...]
//   target = a symbol name (e.g. ShowCursor) OR a hex address (e.g. 0x140942820)
// For each target: resolve to address(es); in 'decomp' mode decompile the function there;
// in 'callers' mode decompile every function that references it (following thunk/IAT
// indirection) and breadcrumb THEIR callers; 'both' does both.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;

public class Trace extends GhidraScript {
    DecompInterface di;
    PrintWriter pw;
    Set<Long> dumped = new HashSet<>();

    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        pw = new PrintWriter(new FileWriter(a[0]));
        String mode = a[1];
        di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);

        for (int i = 2; i < a.length; i++) {
            String t = a[i];
            pw.println("\n#################### TARGET: " + t + " ####################");
            List<Address> addrs = resolve(t);
            if (addrs.isEmpty()) { pw.println("// NOT FOUND: " + t); continue; }
            for (Address ad : addrs) {
                pw.println("// resolved " + t + " -> " + ad);
                if (mode.equals("decomp") || mode.equals("both")) {
                    Function f = getFunctionContaining(ad);
                    if (f != null) decompile(f, "target " + t);
                    else pw.println("// (no function at " + ad + ")");
                }
                if (mode.equals("callers") || mode.equals("both")) {
                    Set<Function> callers = new LinkedHashSet<>();
                    collectCallers(ad, callers, 3);
                    pw.println("// --- " + callers.size() + " caller-functions of " + t + " ---");
                    for (Function f : callers) {
                        decompile(f, "caller of " + t);
                        Set<Function> up = new LinkedHashSet<>();
                        collectCallers(f.getEntryPoint(), up, 3);
                        StringBuilder sb = new StringBuilder("//   ^ callers of " + f.getName() + ": ");
                        for (Function u : up) sb.append(u.getName()).append("@").append(u.getEntryPoint()).append("  ");
                        pw.println(sb.toString());
                    }
                }
            }
        }
        pw.close();
        println("Trace: wrote " + a[0]);
    }

    List<Address> resolve(String t) {
        List<Address> out = new ArrayList<>();
        if (t.startsWith("0x")) { out.add(toAddr(Long.parseLong(t.substring(2), 16))); return out; }
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(t)) out.add(s.getAddress());
        return out;
    }

    void collectCallers(Address target, Set<Function> out, int hops) {
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(target);
        while (ri.hasNext()) {
            Reference r = ri.next();
            Address from = r.getFromAddress();
            Function f = getFunctionContaining(from);
            if (f != null) out.add(f);
            else if (hops > 0) collectCallers(from, out, hops - 1);  // thunk / IAT pointer indirection
        }
    }

    void decompile(Function f, String why) {
        long key = f.getEntryPoint().getOffset();
        if (!dumped.add(key)) { pw.println("// (dup " + f.getName() + " @ " + f.getEntryPoint() + ")"); return; }
        pw.println("\n// ===== " + f.getName() + " @ " + f.getEntryPoint() + "  [" + why + "] =====");
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        if (r != null && r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
        else pw.println("// decompile failed: " + (r != null ? r.getErrorMessage() : "null"));
    }
}
