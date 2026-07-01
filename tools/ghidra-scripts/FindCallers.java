// FindCallers.java - list call/jump xrefs to each target (now that the decrypted
// binary has real references). args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class FindCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        for (int i = 1; i < a.length; i++) {
            long t = Long.parseLong(a[i].replace("0x", ""), 16);
            Address ta = toAddr(t);
            pw.println("== callers of " + a[i] + " ==");
            Reference[] refs = getReferencesTo(ta);
            Set<String> seen = new LinkedHashSet<>();
            for (Reference r : refs) {
                RefType rt = r.getReferenceType();
                if (!rt.isCall() && !rt.isJump()) continue;
                Function f = getFunctionContaining(r.getFromAddress());
                String fe = (f != null) ? ("FUNC 0x" + f.getEntryPoint())
                                        : ("(loose) 0x" + r.getFromAddress());
                seen.add(fe + "   (call site 0x" + r.getFromAddress() + ")");
            }
            int n = 0;
            for (String s : seen) { pw.println("  " + s); if (++n >= 60) { pw.println("  ..."); break; } }
            pw.println("  total xrefs=" + refs.length + ", distinct callers=" + seen.size());
            pw.println();
        }
        pw.close();
        println("FindCallers: wrote " + a[0]);
    }
}
