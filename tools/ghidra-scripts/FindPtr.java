// FindPtr.java - scan all memory for 8-byte little-endian pointers equal to each given address,
// reporting every location (and the function containing it, if any). Used to locate the static
// setting-definition tables that point at INI setting-name strings (which have no code xrefs).
// args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.listing.Function;
import java.io.*;

public class FindPtr extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        Memory mem = currentProgram.getMemory();
        for (int i = 1; i < a.length; i++) {
            long t = Long.parseLong(a[i].replace("0x", ""), 16);
            byte[] pat = new byte[8];
            for (int b = 0; b < 8; b++) pat[b] = (byte) ((t >>> (b * 8)) & 0xFF);
            pw.println("== 8-byte LE pointers to " + a[i] + " ==");
            Address found = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
            int c = 0;
            while (found != null && c < 60) {
                Function f = getFunctionContaining(found);
                pw.println("  @ " + found + (f != null ? ("  in " + f.getName() + " @" + f.getEntryPoint()) : "  (data)"));
                found = mem.findBytes(found.add(1), pat, null, true, monitor);
                c++;
            }
            if (c == 0) pw.println("  (none found)");
            pw.println();
        }
        pw.close();
        println("FindPtr: wrote " + a[0]);
    }
}
