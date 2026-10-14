// Writes "<hex addr> <name>" for every function (unnamed ones as sub_<addr>) to the
// file given as the first script argument. Used by ctr_pc for symbolized logs.
//@category PS4

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ExportFunctions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String path = getScriptArgs()[0];
		int n = 0;
		try (PrintWriter w = new PrintWriter(path, "UTF-8")) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				// default-named functions are kept (as sub_) so lookups stay bounded
				String name = f.getSymbol().getSource() == SourceType.DEFAULT
						? "sub_" + Long.toHexString(f.getEntryPoint().getOffset())
						: f.getName(true);
				w.printf("%x %s%n", f.getEntryPoint().getOffset(), name);
				n++;
			}
		}
		println("exported " + n + " functions");
	}
}
