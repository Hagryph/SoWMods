// DataRefs.java - list every function that references each target address (data refs included,
// unlike FindCallers which only shows call/jump). args: <outfile> <hexaddr> [<hexaddr> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DataRefs extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        for (int i = 1; i < a.length; i++) {
            long t = Long.parseLong(a[i].replace("0x", ""), 16);
            Address ta = toAddr(t);
            pw.println("== refs to " + a[i] + " ==");
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(ta);
            Set<String> seen = new LinkedHashSet<>();
            while (ri.hasNext()) {
                Reference r = ri.next();
                Function f = getFunctionContaining(r.getFromAddress());
                String fe = (f != null) ? ("FUNC 0x" + f.getEntryPoint() + " " + f.getName())
                                        : ("(loose) 0x" + r.getFromAddress());
                seen.add(fe + "  [" + r.getReferenceType() + " @0x" + r.getFromAddress() + "]");
            }
            for (String s : seen) pw.println("  " + s);
            pw.println("  total distinct: " + seen.size());
            pw.println();
        }
        pw.close();
        println("DataRefs: wrote " + a[0]);
    }
}
