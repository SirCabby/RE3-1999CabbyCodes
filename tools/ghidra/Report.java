// Headless reporting over the analysed dump: what refers to an address, and the
// decompilation of the functions involved.
//
//   analyzeHeadless ghidra_proj RE3 -process ResidentEvil3.fixed.exe -noanalysis \
//       -scriptPath tools/ghidra -postScript Report.java xref 0x514570 0x516810
//       -postScript Report.java dec 0x4815F0
//       -postScript Report.java funcs            (every function: address, name, size)
//       -postScript Report.java strings 0x514000 0x520000
//
// "xref" lists every reference to each address with the function it sits in;
// "dec" decompiles the function containing each address.
//@category RE3
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class Report extends GhidraScript {
  private DecompInterface ifc;
  private PrintWriter out;

  private void say(String s) {
    if (out != null) out.println(s);
    else println(s);
  }

  private void decompile(Address addr) throws Exception {
    Function f = getFunctionContaining(addr);
    if (f == null) f = createFunction(addr, null);
    if (f == null) {
      say("=== " + addr + ": no function ===");
      return;
    }
    DecompileResults r = ifc.decompileFunction(f, 120, monitor);
    say("=== " + f.getName() + " @ " + f.getEntryPoint() + " (asked for " + addr + ") ===");
    if (r != null && r.getDecompiledFunction() != null) say(r.getDecompiledFunction().getC());
    else say("(decompilation failed)");
  }

  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    if (args.length < 2) {
      println("usage: Report.java <out-file> <xref|dec|funcs> [addresses...]");
      return;
    }
    ifc = new DecompInterface();
    ifc.openProgram(currentProgram);
    out = new PrintWriter(args[0]);
    String mode = args[1];
    if (mode.equals("funcs")) {
      FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
      while (it.hasNext()) {
        Function f = it.next();
        say("FUNC " + f.getEntryPoint() + " " + f.getBody().getNumAddresses() + " " + f.getName());
      }
      out.close();
      ifc.dispose();
      return;
    }
    for (int i = 2; i < args.length; ++i) {
      String arg = args[i];
      String m = mode;
      // "x:0x401000" / "d:0x401000" override the mode for one argument, so one
      // run can ask several questions (each run reopens the project).
      if (arg.length() > 2 && arg.charAt(1) == ':') {
        m = arg.charAt(0) == 'x' ? "xref" : "dec";
        arg = arg.substring(2);
      }
      long v = Long.parseLong(arg.replace("0x", "").replace("0X", ""), 16);
      Address addr = toAddr(v);
      if (m.equals("dec")) {
        decompile(addr);
        continue;
      }
      say("=== references to " + addr + " ===");
      ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(addr);
      int n = 0;
      while (refs.hasNext()) {
        Reference r = refs.next();
        Address from = r.getFromAddress();
        Function f = getFunctionContaining(from);
        say("  " + from + " " + r.getReferenceType() + (f != null ? "  in " + f.getName() + " @ " + f.getEntryPoint() : ""));
        ++n;
      }
      if (n == 0) say("  (none)");
    }
    out.close();
    ifc.dispose();
  }
}
