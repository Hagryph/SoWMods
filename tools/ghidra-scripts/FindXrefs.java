// FindXrefs.java - list functions that CALL a target address, and decompile them.
// args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class FindXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();

        for (int i = 1; i < a.length; i++) {
            long addr = Long.parseLong(a[i].replace("0x", ""), 16);
            Address ad = toAddr(addr);
            pw.println("// ===== callers of " + a[i] + " =====");
            Set<Function> callers = new LinkedHashSet<>();
            for (Reference r : rm.getReferencesTo(ad)) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f != null) callers.add(f);
            }
            pw.println("// caller count: " + callers.size());
            for (Function f : callers) {
                pw.println("// ---- caller @" + f.getEntryPoint() + " ----");
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                if (r != null && r.decompileCompleted())
                    pw.println(r.getDecompiledFunction().getC());
                else
                    pw.println("// decompile failed");
            }
        }
        pw.close();
        println("FindXrefs: wrote " + a[0]);
    }
}
