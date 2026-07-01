// ProbeRefs.java - is the reference DB populated at all? code refs vs data refs.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class ProbeRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("== ref counts to known addresses ==");
        long[] probes = { 0x141a00570L /*ScaleformFileOpener err*/,
                          0x1419ffcb0L /*Interface/Exported/%s.gfx*/,
                          0x1420a8bf8L /*InventoryMenu RTTI typedesc*/,
                          0x141a09d30L /*GFxLoader read failed*/ };
        for (long p : probes) {
            Address a = toAddr(p);
            println(String.format("  0x%x : refCountTo=%d", p, rm.getReferenceCountTo(a)));
        }

        println("\n== total functions + caller-ref sampling ==");
        int total = fm.getFunctionCount();
        println("  function count = " + total);
        FunctionIterator it = fm.getFunctions(true);
        int i = 0, withCallers = 0, withRefsFrom = 0;
        long totalRefsFrom = 0;
        while (it.hasNext() && i < 400) {
            Function f = it.next();
            i++;
            Address e = f.getEntryPoint();
            int callers = rm.getReferenceCountTo(e);
            if (callers > 0) withCallers++;
            // outgoing refs from the function body (sample first instruction range)
            int rf = 0;
            InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                rf += ins.getReferencesFrom().length;
            }
            totalRefsFrom += rf;
            if (rf > 0) withRefsFrom++;
            if (i <= 8) println(String.format("  fn 0x%s callers=%d refsFromBody=%d",
                e, callers, rf));
        }
        println(String.format("  of first %d fns: %d have callers, %d have outgoing refs, total outgoing=%d",
            i, withCallers, withRefsFrom, totalRefsFrom));
    }
}
