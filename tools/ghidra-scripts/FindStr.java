// FindStr.java - find defined strings matching any of the given substrings (case-insensitive),
// print the string + address, and list every function that references each string.
//   args: <outfile> <needle> [<needle> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;

public class FindStr extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        List<String> needles = new ArrayList<>();
        for (int i = 1; i < a.length; i++) needles.add(a[i].toLowerCase());

        Listing listing = currentProgram.getListing();
        DataIterator di = listing.getDefinedData(true);
        int hits = 0;
        while (di.hasNext() && hits < 4000) {
            Data d = di.next();
            DataType dt = d.getDataType();
            String tn = dt.getName().toLowerCase();
            if (!(tn.contains("unicode") || tn.contains("string") || tn.contains("char"))) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            String sl = s.toLowerCase();
            boolean match = false;
            for (String n : needles) if (sl.contains(n)) { match = true; break; }
            if (!match) continue;
            hits++;
            pw.println("\n=== STR @ " + d.getAddress() + " : \"" + s.replace("\n","\\n") + "\"");
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
            int rc = 0;
            while (ri.hasNext() && rc < 40) {
                rc++;
                Reference r = ri.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                pw.println("   <- ref from " + from + (f != null ? ("  in " + f.getName() + " @ " + f.getEntryPoint()) : "  (no func)"));
            }
            if (rc == 0) pw.println("   (no refs)");
        }
        pw.println("\n// total string hits: " + hits);
        pw.close();
        println("FindStr: wrote " + a[0] + " hits=" + hits);
    }
}
