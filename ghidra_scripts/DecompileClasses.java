// Decompiles every function in the given class namespaces (plus direct callees up to
// a depth) into one C file.
// args: <out.c> <depth> <Class1> [Class2 ...]   (a "0x1234" arg adds a single function)
//@category PS4

import java.io.PrintWriter;
import java.util.*;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;

public class DecompileClasses extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		String out = args[0];
		int depth = Integer.parseInt(args[1]);
		LinkedHashSet<Function> todo = new LinkedHashSet<>();
		for (int i = 2; i < args.length; i++) {
			if (args[i].startsWith("0x")) {
				Function f = getFunctionAt(toAddr(Long.parseUnsignedLong(args[i].substring(2), 16)));
				if (f != null) {
					todo.add(f);
				}
				continue;
			}
			Namespace ns = getNamespace(null, args[i]);
			if (ns == null) {
				printerr("no namespace " + args[i]);
				continue;
			}
			for (Symbol s : currentProgram.getSymbolTable().getSymbols(ns)) {
				Function f = getFunctionAt(s.getAddress());
				if (f != null) {
					todo.add(f);
				}
			}
		}
		Set<Function> all = new LinkedHashSet<>(todo);
		Set<Function> frontier = new LinkedHashSet<>(todo);
		for (int d = 0; d < depth; d++) {
			Set<Function> next = new LinkedHashSet<>();
			for (Function f : frontier) {
				for (Function c : f.getCalledFunctions(monitor)) {
					if (c.isThunk() || c.getEntryPoint().getOffset() >= 0x3000000L) {
						continue;
					}
					if (all.add(c)) {
						next.add(c);
					}
				}
			}
			frontier = next;
		}

		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int n = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			for (Function f : all) {
				monitor.checkCancelled();
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				w.printf("// ===== %s @ 0x%x%n", f.getName(true), f.getEntryPoint().getOffset());
				if (r != null && r.decompileCompleted()) {
					w.println(r.getDecompiledFunction().getC());
				} else {
					w.println("// decompile failed");
				}
				n++;
			}
		}
		di.dispose();
		println("decompiled " + n + " functions");
	}
}
