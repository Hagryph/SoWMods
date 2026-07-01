import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class ProbeOne extends GhidraScript {
    @Override
    public void run() throws Exception {
        long[] fns = { 0x14002bb30L, 0x140195ba0L, 0x14164f644L };
        for (long fa : fns) {
            Address fn = toAddr(fa);
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < 24; i++) hex.append(String.format("%02x ", getByte(fn.add(i)) & 0xFF));
            println(String.format("0x%x bytes: %s", fa, hex));

            clearListing(fn, fn.add(0x400));
            boolean ok = disassemble(fn);

            int c = 0;
            Address p = fn;
            while (p.compareTo(fn.add(0x400)) < 0) {
                Instruction ins = getInstructionAt(p);
                if (ins == null) {
                    println(String.format("  STOP: undefined at 0x%s (byte 0x%02x)", p, getByte(p) & 0xFF));
                    break;
                }
                if (c < 6) println(String.format("    0x%s  %s", p, ins.toString()));
                c++;
                p = p.add(ins.getLength());
            }
            println(String.format("  disassemble ok=%b  instrs(in 0x400)=%d", ok, c));
            println("");
        }
    }
}
