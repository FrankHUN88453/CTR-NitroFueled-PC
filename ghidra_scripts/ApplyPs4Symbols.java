// Applies the symbol list produced by tools/prep_ghidra.py.
// Run as a pre-script (before auto analysis) with the .tsv path as argument.
//@category PS4

import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class ApplyPs4Symbols extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String path = getScriptArgs().length > 0 ? getScriptArgs()[0]
				: askFile("symbols.tsv", "Open").getAbsolutePath();

		AddressSet starts = new AddressSet();
		int imports = 0, funcs = 0;
		try (BufferedReader r = new BufferedReader(new FileReader(path))) {
			String line;
			while ((line = r.readLine()) != null) {
				String[] f = line.split(" ");
				Address a = toAddr(Long.parseUnsignedLong(f[1], 16));
				switch (f[0]) {
					case "I": {
						Namespace ns = getOrCreateNamespace(f[3]);
						Function fn = createFunction(a, null);
						if (fn == null) {
							fn = getFunctionAt(a);
						}
						if (fn != null) {
							fn.setName(f[2], SourceType.IMPORTED);
							fn.setParentNamespace(ns);
						}
						else {
							createLabel(a, f[2], ns, true, SourceType.IMPORTED);
						}
						imports++;
						break;
					}
					case "E":
						addEntryPoint(a);
						createLabel(a, f[2], true, SourceType.IMPORTED);
						starts.add(a);
						break;
					case "F":
						starts.add(a);
						funcs++;
						break;
				}
			}
		}

		monitor.setMessage("Disassembling " + funcs + " eh_frame function starts");
		new DisassembleCommand(starts, null, true).applyTo(currentProgram, monitor);
		monitor.initialize(starts.getNumAddresses());
		for (Address a : starts.getAddresses(true)) {
			monitor.checkCancelled();
			monitor.incrementProgress(1);
			if (getFunctionAt(a) == null) {
				createFunction(a, null);
			}
		}
		println("imports: " + imports + ", function starts: " + funcs);
	}

	private Namespace getOrCreateNamespace(String name) throws Exception {
		Namespace ns = getNamespace(null, name);
		return ns != null ? ns
				: currentProgram.getSymbolTable().createNameSpace(null, name, SourceType.IMPORTED);
	}
}
