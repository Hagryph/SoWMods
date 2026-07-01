// DisassemblePdata2.java - forceful: for each .pdata RUNTIME_FUNCTION, clear the
// [begin,end) byte range (removes data that blocks disassembly) then disassemble.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.mem.MemoryBlock;

public class DisassemblePdata2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        MemoryBlock pdata = currentProgram.getMemory().getBlock(".pdata");
        if (pdata == null) { println("no .pdata"); return; }

        println("instructions BEFORE = " + currentProgram.getListing().getNumInstructions());

        AddressSet starts = new AddressSet();
        Address a = pdata.getStart(), end = pdata.getEnd();
        int count = 0, cleared = 0, dis = 0;
        while (a.compareTo(end) < 0 && !monitor.isCancelled()) {
            int beginRVA, endRVA;
            try { beginRVA = getInt(a); endRVA = getInt(a.add(4)); }
            catch (Exception e) { break; }
            a = a.add(12);
            if (beginRVA == 0 || endRVA <= beginRVA) continue;
            count++;
            Address s = toAddr(base + (beginRVA & 0xFFFFFFFFL));
            Address e = toAddr(base + (endRVA & 0xFFFFFFFFL) - 1);
            starts.addRange(s, s);
            if (getInstructionAt(s) != null) continue;   // already code
            try {
                clearListing(s, e);                       // wipe blocking data
                cleared++;
                if (disassemble(s)) dis++;                // follow flow
            } catch (Exception ex) { /* skip */ }
        }
        println("entries=" + count + " cleared=" + cleared + " disassembledOK=" + dis);

        println("creating functions...");
        int made = 0;
        for (Address s : starts.getAddresses(true)) {
            if (monitor.isCancelled()) break;
            if (getFunctionAt(s) != null) continue;
            try { if (new CreateFunctionCmd(s).applyTo(currentProgram, monitor)) made++; }
            catch (Exception ex) { }
        }
        println("new functions = " + made);
        println("instructions AFTER  = " + currentProgram.getListing().getNumInstructions());
    }
}
