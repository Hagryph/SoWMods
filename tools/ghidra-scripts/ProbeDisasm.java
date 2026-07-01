import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class ProbeDisasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        println("numInstructions = " + l.getNumInstructions());
        println("numDefinedData  = " + l.getNumDefinedData());
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(String.format("block %-12s %s - %s  exec=%b  init=%b  size=%d",
                b.getName(), b.getStart(), b.getEnd(), b.isExecute(), b.isInitialized(), b.getSize()));
        }
        FunctionManager fm = currentProgram.getFunctionManager();
        long[] fns = { 0x14002bb30L, 0x140195c16L, 0x14164f644L, 0x140ae5d00L };
        for (long a : fns) {
            Function f = fm.getFunctionContaining(toAddr(a));
            if (f == null) { println(String.format("0x%x : NO function", a)); continue; }
            int c = 0;
            InstructionIterator it = l.getInstructions(f.getBody(), true);
            while (it.hasNext()) { it.next(); c++; }
            println(String.format("0x%x : entry=0x%s bodyAddrs=%d instrs=%d",
                a, f.getEntryPoint(), f.getBody().getNumAddresses(), c));
        }
        // sample: how much of .text is instructions vs undefined?
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        if (text != null) {
            long instrBytes = 0;
            InstructionIterator it = l.getInstructions(text.getStart(), true);
            int n = 0;
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getAddress().compareTo(text.getEnd()) > 0) break;
                instrBytes += ins.getLength();
                n++;
            }
            println(String.format(".text size=%d  instrBytes=%d  (%.1f%% disassembled)  instrs=%d",
                text.getSize(), instrBytes, 100.0 * instrBytes / text.getSize(), n));
        }
    }
}
