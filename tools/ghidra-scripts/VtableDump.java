// VtableDump.java - dump N vtable slots (pointers) at an address and decompile each.
//   args: <outfile> <vtableHexAddr> <count>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import java.io.*;

public class VtableDump extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        long base = Long.parseLong(a[1].replace("0x", ""), 16);
        int n = Integer.parseInt(a[2]);
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        Memory mem = currentProgram.getMemory();
        for (int i = 0; i < n; i++) {
            Address slot = toAddr(base + (long) i * 8);
            long fp = mem.getLong(slot);
            pw.println("\n// ===== vtable[" + i + "] (off +0x" + Integer.toHexString(i * 8)
                + ") @" + slot + " -> 0x" + Long.toHexString(fp) + " =====");
            Function f = getFunctionContaining(toAddr(fp));
            if (f == null) { pw.println("// (no function at target)"); continue; }
            DecompileResults r = di.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
            else pw.println("// decompile failed");
        }
        pw.close();
        println("VtableDump: wrote " + a[0]);
    }
}
