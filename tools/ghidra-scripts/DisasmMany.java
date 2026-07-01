// DisasmMany.java - disassemble `count` instructions at each address, with raw bytes, to a file.
// args: <outfile> then repeating pairs: <hexaddr> <count>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import java.io.*;

public class DisasmMany extends GhidraScript {
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        for (int i = 1; i + 1 < a.length; i += 2) {
            long addr = Long.parseLong(a[i].replace("0x", ""), 16);
            int n = Integer.parseInt(a[i + 1]);
            Address p = toAddr(addr);
            pw.println("== " + a[i] + " ==");
            for (int k = 0; k < n && p != null; k++) {
                Instruction ins = getInstructionAt(p);
                if (ins == null) { disassemble(p); ins = getInstructionAt(p); }
                if (ins == null) { pw.println("  0x" + p + "  (no instruction)"); break; }
                byte[] b = ins.getBytes();
                StringBuilder hex = new StringBuilder();
                for (byte bb : b) hex.append(String.format("%02x", bb & 0xFF));
                pw.println(String.format("  0x%s  %-42s %s", p, ins.toString(), hex));
                p = p.add(ins.getLength());
            }
            pw.println();
        }
        pw.close();
        println("DisasmMany: wrote " + a[0]);
    }
}
