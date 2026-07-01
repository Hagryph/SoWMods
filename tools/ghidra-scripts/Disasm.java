// Disasm.java - dump N instructions at an address (disassembling if needed).
// args: <hexAddr> [count]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long addr = Long.parseLong(a[0].replace("0x", ""), 16);
        int n = (a.length > 1) ? Integer.parseInt(a[1]) : 30;
        Address p = toAddr(addr);
        for (int i = 0; i < n && p != null; i++) {
            Instruction ins = getInstructionAt(p);
            if (ins == null) { disassemble(p); ins = getInstructionAt(p); }
            if (ins == null) { println(String.format("0x%s  (no instruction)", p)); break; }
            String tgt = "";
            Address[] flows = ins.getFlows();
            if (flows != null && flows.length > 0) tgt = "  -> 0x" + flows[0];
            println(String.format("0x%s  %-32s%s", p, ins.toString(), tgt));
            p = p.add(ins.getLength());
        }
    }
}
