// RTTI.java - locate a class via its RTTI type-descriptor string (".?AVName@@"),
// follow ColRef -> CompleteObjectLocator -> vtable, and dump vtable refs.
// Simpler approach: find the type-descriptor data, find what references it (the
// RTTICompleteObjectLocator), then find what references THAT (the vtable start),
// then find code that loads the vtable address (ctor/placement).
//   args: <outfile> <typeDescriptorString> [<more strings>...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;

public class RTTI extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        ReferenceManager rm = currentProgram.getReferenceManager();

        for (int ai = 1; ai < a.length; ai++) {
            String needle = a[ai];
            pw.println("\n############ RTTI: " + needle + " ############");
            Address tdAddr = null;
            DataIterator di = listing.getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                Object v = d.getValue();
                if (v == null) continue;
                if (v.toString().contains(needle)) {
                    // type descriptor string sits at +0x10 inside the TypeDescriptor struct
                    tdAddr = d.getAddress();
                    pw.println("// type-descriptor string @ " + tdAddr + " : " + v);
                    break;
                }
            }
            if (tdAddr == null) { pw.println("// NOT FOUND"); continue; }
            // TypeDescriptor begins 0x10 before the name string
            Address tdBase = tdAddr.subtract(0x10);
            pw.println("// TypeDescriptor base ~ " + tdBase);
            // find refs to the TypeDescriptor base (the COL's pTypeDescriptor field, an image-relative or absolute)
            for (Address probe : new Address[]{ tdBase, tdAddr }) {
                ReferenceIterator ri = rm.getReferencesTo(probe);
                int c = 0;
                while (ri.hasNext() && c < 25) {
                    c++;
                    Reference r = ri.next();
                    Address from = r.getFromAddress();
                    pw.println("   TD<-ref @ " + from);
                    // each such ref is inside a CompleteObjectLocator; find refs to the COL
                    // COL starts 0x0c before pTypeDescriptor field on x64 (signature,offset,cdOffset,pTD)
                    Address colBase = from.subtract(0x0c);
                    ReferenceIterator ri2 = rm.getReferencesTo(colBase);
                    while (ri2.hasNext()) {
                        Reference r2 = ri2.next();
                        Address colRefFrom = r2.getFromAddress();
                        // vtable is colRefFrom + 8
                        Address vtbl = colRefFrom.add(8);
                        pw.println("      COL<-ref @ " + colRefFrom + "  => vtable @ " + vtbl);
                        ReferenceIterator ri3 = rm.getReferencesTo(vtbl);
                        while (ri3.hasNext()) {
                            Reference r3 = ri3.next();
                            Function f = getFunctionContaining(r3.getFromAddress());
                            pw.println("         vtable<-ref @ " + r3.getFromAddress()
                                + (f != null ? ("  in " + f.getName() + " @ " + f.getEntryPoint()) : ""));
                        }
                    }
                }
            }
        }
        pw.close();
        println("RTTI: wrote " + a[0]);
    }
}
