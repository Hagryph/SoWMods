// Probe2.java - find who references the BarterMenu creator (-> UI::Register / registry)
// and dump the IMenu vtable contents.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;

public class Probe2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));

        long creator = 0x1408ef2b0L;
        pw.println("== references to BarterMenu creator 0x1408ef2b0 ==");
        for (Reference r : getReferencesTo(toAddr(creator))) {
            Function f = getFunctionContaining(r.getFromAddress());
            pw.println("  " + r.getReferenceType() + " from 0x" + r.getFromAddress()
                + (f != null ? "  in FUNC 0x" + f.getEntryPoint() : "  [data]"));
        }

        pw.println("\n== IMenu vtable @0x1418f0ce0 (BarterMenu) ==");
        long vt = 0x1418f0ce0L;
        for (int i = 0; i < 35; i++) {
            try {
                Address slot = toAddr(vt + i * 8L);
                long ptr = getLong(slot);
                Function f = getFunctionAt(toAddr(ptr));
                pw.println(String.format("  [+0x%02x] -> 0x%x  %s", i * 8, ptr, f != null ? f.getName() : ""));
            } catch (Exception e) { pw.println("  [+0x" + Integer.toHexString(i*8) + "] (unreadable)"); break; }
        }

        // BSScaleformManager singleton + allocator sanity
        pw.println("\n== singletons ==");
        pw.println("  BSScaleformManager ptr global = 0x1435f11c8");
        pw.println("  menu allocator global         = 0x143292490");

        pw.close();
        println("Probe2: wrote " + a[0]);
    }
}
