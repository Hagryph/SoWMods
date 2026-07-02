// ClassOf.java - given vtable addresses, resolve their MSVC x64 RTTI class name by following
//   vtable[-1] -> CompleteObjectLocator -> (+0xC) TypeDescriptor RVA -> (+0x10) mangled name.
// args: <outfile> <vtableHex> [<vtableHex> ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.io.*;

public class ClassOf extends GhidraScript {
    long u32(Address a) throws Exception { return currentProgram.getMemory().getInt(a) & 0xFFFFFFFFL; }
    long u64(Address a) throws Exception { return currentProgram.getMemory().getLong(a); }

    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
        long imageBase = 0x140000000L;
        for (int i = 1; i < a.length; i++) {
            long vt = Long.parseLong(a[i].replace("0x", ""), 16);
            pw.print(a[i] + " -> ");
            try {
                long col   = u64(toAddr(vt - 8));            // CompleteObjectLocator*
                long tdRva = u32(toAddr(col + 0xC));         // TypeDescriptor RVA (image-relative)
                Address nm = toAddr(imageBase + tdRva).add(0x10);
                StringBuilder sb = new StringBuilder();
                for (int k = 0; k < 160; k++) { byte b = currentProgram.getMemory().getByte(nm.add(k)); if (b == 0) break; sb.append((char) b); }
                pw.println("name='" + sb + "'   (col=0x" + Long.toHexString(col) + ")");
            } catch (Exception e) { pw.println("(no RTTI here: " + e.getMessage() + ")"); }
        }
        pw.close();
        println("ClassOf: wrote " + a[0]);
    }
}
