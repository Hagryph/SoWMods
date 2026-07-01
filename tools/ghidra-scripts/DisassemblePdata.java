// DisassemblePdata.java - use the x64 .pdata (RUNTIME_FUNCTION[]) table to
// disassemble + create every function Ghidra's flow analysis missed.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;

public class DisassemblePdata extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        MemoryBlock pdata = currentProgram.getMemory().getBlock(".pdata");
        if (pdata == null) { println("no .pdata block"); return; }

        println("instructions BEFORE = " + currentProgram.getListing().getNumInstructions());

        AddressSet starts = new AddressSet();
        Address a = pdata.getStart();
        Address end = pdata.getEnd();
        int count = 0;
        while (a.compareTo(end) < 0) {
            int beginRVA;
            try { beginRVA = getInt(a); } catch (Exception e) { break; }
            if (beginRVA != 0) {
                starts.addRange(toAddr(base + (beginRVA & 0xFFFFFFFFL)),
                                toAddr(base + (beginRVA & 0xFFFFFFFFL)));
                count++;
            }
            a = a.add(12);  // sizeof(RUNTIME_FUNCTION)
        }
        println("RUNTIME_FUNCTION entries = " + count);

        println("disassembling (follow flow from every entry)...");
        DisassembleCommand cmd = new DisassembleCommand(starts, null, true);
        cmd.applyTo(currentProgram, monitor);

        println("creating functions...");
        int made = 0, i = 0;
        for (Address s : starts.getAddresses(true)) {
            i++;
            if (monitor.isCancelled()) break;
            if (getFunctionAt(s) != null) continue;
            try {
                CreateFunctionCmd cf = new CreateFunctionCmd(s);
                if (cf.applyTo(currentProgram, monitor)) made++;
            } catch (Exception e) { /* skip bad entries */ }
        }
        println("entries processed = " + i + ", new functions = " + made);
        println("instructions AFTER  = " + currentProgram.getListing().getNumInstructions());
    }
}
