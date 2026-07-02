// FindOffsetRef.java - scan all instructions for a given struct-member displacement (e.g. 0x53f8)
// appearing as an operand, and report the containing function. Finds every accessor of that member;
// the WRITE sites (MOV [reg+off], ...) are the populator/initializer.
//   args: <outfile> <hexOffset> [maxHits]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;

public class FindOffsetRef extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        long target = Long.parseLong(a[1].replace("0x", ""), 16);
        int max = (a.length > 2) ? Integer.parseInt(a[2]) : 400;
        Listing lst = currentProgram.getListing();
        InstructionIterator it = lst.getInstructions(true);
        int hits = 0;
        Set<String> funcs = new LinkedHashSet<>();
        while (it.hasNext() && hits < max) {
            Instruction ins = it.next();
            boolean match = false;
            for (int op = 0; op < ins.getNumOperands() && !match; op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == target) { match = true; break; }
                }
            }
            if (!match) continue;
            hits++;
            Function f = getFunctionContaining(ins.getAddress());
            String mn = ins.getMnemonicString();
            boolean write = mn.startsWith("MOV") && ins.toString().indexOf("[") < ins.toString().indexOf(",");
            pw.println((write ? "W " : "  ") + ins.getAddress() + "  " + ins
                + (f != null ? ("   in " + f.getName() + " @ " + f.getEntryPoint()) : ""));
            if (f != null) funcs.add(f.getName() + " @ " + f.getEntryPoint());
        }
        pw.println("\n// distinct functions touching +0x" + Long.toHexString(target) + ":");
        for (String s : funcs) pw.println("//   " + s);
        pw.println("// total hits: " + hits);
        pw.close();
        println("FindOffsetRef: wrote " + a[0] + " hits=" + hits);
    }
}
