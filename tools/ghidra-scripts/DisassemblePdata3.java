// DisassemblePdata3.java - clear ALL of .text to undefined, then disassemble
// fresh from every .pdata function entry. Removes the data mis-marking wholesale.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;

public class DisassemblePdata3 extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");   // big code block
        MemoryBlock pdata = currentProgram.getMemory().getBlock(".pdata");
        if (text == null || pdata == null) { println("missing block"); return; }

        println("instructions BEFORE = " + currentProgram.getListing().getNumInstructions());
        println("clearing .text " + text.getStart() + " - " + text.getEnd() + " ...");
        clearListing(text.getStart(), text.getEnd());

        AddressSet starts = new AddressSet();
        Address a = pdata.getStart(), end = pdata.getEnd();
        int count = 0;
        while (a.compareTo(end) < 0) {
            int beginRVA;
            try { beginRVA = getInt(a); } catch (Exception e) { break; }
            a = a.add(12);
            if (beginRVA == 0) continue;
            Address s = toAddr(base + (beginRVA & 0xFFFFFFFFL));
            if (s.compareTo(text.getStart()) >= 0 && s.compareTo(text.getEnd()) <= 0) {
                starts.addRange(s, s);
                count++;
            }
        }
        println("pdata entries in .text = " + count);

        println("disassembling (follow flow from every entry)...");
        DisassembleCommand cmd = new DisassembleCommand(starts, null, true);
        cmd.applyTo(currentProgram, monitor);
        println("instructions after disasm = " + currentProgram.getListing().getNumInstructions());

        println("creating functions...");
        int made = 0;
        for (Address s : starts.getAddresses(true)) {
            if (monitor.isCancelled()) break;
            if (getFunctionAt(s) != null) continue;
            try { if (new CreateFunctionCmd(s).applyTo(currentProgram, monitor)) made++; }
            catch (Exception e) { }
        }
        println("functions created = " + made);
        println("instructions FINAL = " + currentProgram.getListing().getNumInstructions());
    }
}
