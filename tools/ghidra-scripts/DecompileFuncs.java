// DecompileFuncs.java - decompile a list of functions to C pseudocode.
// args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import java.io.*;

public class DecompileFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            long addr = Long.parseLong(a[i].replace("0x", ""), 16);
            Address ad = toAddr(addr);
            Function f = getFunctionContaining(ad);
            pw.println("// ================= " + a[i] + "  func@"
                + (f != null ? f.getEntryPoint() : "?") + " =================");
            if (f == null) { pw.println("// (no function here)\n"); continue; }
            DecompileResults r = di.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) {
                pw.println(r.getDecompiledFunction().getC());
            } else {
                pw.println("// decompile failed: " + (r != null ? r.getErrorMessage() : "null"));
            }
            pw.println();
        }
        pw.close();
        println("DecompileFuncs: wrote " + a[0]);
    }
}
