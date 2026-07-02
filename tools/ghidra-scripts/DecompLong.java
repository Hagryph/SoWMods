// DecompLong.java - decompile one or more functions with a long (600s) timeout, for big functions
// that exceed Trace.java's 120s limit. Also lists each function's direct callees (FUN_ targets).
//   args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;

public class DecompLong extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Address ad = toAddr(Long.parseLong(a[i].replace("0x", ""), 16));
            Function f = getFunctionContaining(ad);
            pw.println("\n// ===== " + (f != null ? f.getName() : "?") + " @ " + ad + " =====");
            if (f == null) { pw.println("// no function"); continue; }
            DecompileResults r = di.decompileFunction(f, 600, monitor);
            if (r != null && r.decompileCompleted())
                pw.println(r.getDecompiledFunction().getC());
            else
                pw.println("// decompile failed: " + (r != null ? r.getErrorMessage() : "null"));
        }
        pw.close();
        println("DecompLong: wrote " + a[0]);
    }
}
