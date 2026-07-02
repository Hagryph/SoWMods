// FindImport.java - locate an imported API (external symbol, thunk, IAT pointer) and list every
// reference with its containing function, so we can pin the engine call site of a Win32 API.
//   args: <outfile> <apiName> [<apiName> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class FindImport extends GhidraScript {
    void refs(PrintWriter pw, Address ta) {
        Reference[] rs = getReferencesTo(ta);
        pw.println("    refs=" + rs.length);
        int n = 0;
        for (Reference r : rs) {
            Function f = getFunctionContaining(r.getFromAddress());
            pw.println("    <- " + r.getReferenceType() + " from " + r.getFromAddress()
                       + (f != null ? "  in FUNC @ " + f.getEntryPoint() + " " + f.getName() : "  (no func)"));
            if (++n >= 100) { pw.println("    ..."); break; }
        }
    }
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        SymbolTable st = currentProgram.getSymbolTable();
        for (int i = 1; i < a.length; i++) {
            pw.println("== " + a[i] + " ==");
            SymbolIterator si = st.getSymbolIterator("*" + a[i] + "*", true);
            int hits = 0;
            while (si.hasNext() && hits < 40) {
                Symbol s = si.next();
                hits++;
                pw.println("  sym \"" + s.getName() + "\"  type=" + s.getSymbolType()
                           + "  @ " + s.getAddress() + (s.isExternal() ? "  (external)" : ""));
                refs(pw, s.getAddress());
            }
            if (hits == 0) pw.println("  (no symbols matched)");
            pw.println();
        }
        pw.close();
        println("FindImport: wrote " + a[0]);
    }
}
