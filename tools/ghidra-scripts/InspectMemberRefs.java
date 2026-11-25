// Inspect instructions following member accesses from FindOffsetRef output.
// args: <outfile> <FindOffsetRef-output> [instruction-count]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.regex.*;

public class InspectMemberRefs extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int count = a.length > 2 ? Integer.parseInt(a[2]) : 10;
        Pattern pattern = Pattern.compile("^[ W]*([0-9a-f]+)  .*");
        try (BufferedReader input = new BufferedReader(new FileReader(a[1]));
             PrintWriter output = new PrintWriter(new FileWriter(a[0]))) {
            String line;
            while ((line = input.readLine()) != null) {
                Matcher m = pattern.matcher(line);
                if (!m.matches()) continue;
                long value = Long.parseLong(m.group(1), 16);
                if (value >= 0x142000000L) continue;
                Address address = toAddr(value);
                Function f = getFunctionContaining(address);
                output.println("\n=== " + address + " " + (f != null ? f.getEntryPoint() : "unknown"));
                Instruction instruction = getInstructionAt(address);
                for (int i = 0; i < count && instruction != null; ++i) {
                    output.println(instruction.getAddress() + "  " + instruction);
                    instruction = instruction.getNext();
                }
            }
        }
    }
}
