// Applies out/ghidra_alchemy.json (tools/gen_alchemy_types.py): Alchemy class
// structs, class namespaces, vtables, registration functions, virtual functions.
//@category PS4

import java.io.FileReader;
import java.util.HashMap;
import java.util.Map;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.util.exception.DuplicateNameException;

public class ApplyAlchemyMeta extends GhidraScript {

	private final CategoryPath CAT = new CategoryPath("/Alchemy");
	private final Map<String, Namespace> classNs = new HashMap<>();
	private DataTypeManager dtm;

	@Override
	protected void run() throws Exception {
		String path = getScriptArgs().length > 0 ? getScriptArgs()[0]
				: askFile("ghidra_alchemy.json", "Open").getAbsolutePath();
		JsonObject root;
		try (FileReader r = new FileReader(path)) {
			root = JsonParser.parseReader(r).getAsJsonObject();
		}
		dtm = currentProgram.getDataTypeManager();

		JsonArray structs = root.getAsJsonArray("structs");
		monitor.initialize(structs.size() * 2L);
		monitor.setMessage("Creating structs");
		Map<String, Structure> created = new HashMap<>();
		for (JsonElement e : structs) {
			JsonObject s = e.getAsJsonObject();
			String name = s.get("name").getAsString();
			int size = Math.max(s.get("size").getAsInt(), 1);
			Structure st = new StructureDataType(CAT, name, size, dtm);
			created.put(name, (Structure) dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER));
			monitor.incrementProgress(1);
		}
		monitor.setMessage("Filling structs");
		for (JsonElement e : structs) {
			monitor.checkCancelled();
			JsonObject s = e.getAsJsonObject();
			Structure st = created.get(s.get("name").getAsString());
			for (JsonElement me : s.getAsJsonArray("members")) {
				JsonObject m = me.getAsJsonObject();
				int off = m.get("offset").getAsInt();
				int len = m.get("size").getAsInt();
				if (off + len > st.getLength()) {
					continue;
				}
				DataType dt = parseType(m.get("ctype").getAsString(), len);
				try {
					st.replaceAtOffset(off, dt, len, m.get("name").getAsString(),
						m.get("meta").getAsString());
				}
				catch (IllegalArgumentException ex) {
					printerr(st.getName() + "+" + off + ": " + ex.getMessage());
				}
			}
			monitor.incrementProgress(1);
		}

		JsonArray labels = root.getAsJsonArray("labels");
		monitor.initialize(labels.size());
		monitor.setMessage("Labels and functions");
		int funcs = 0;
		for (JsonElement e : labels) {
			monitor.checkCancelled();
			monitor.incrementProgress(1);
			JsonObject l = e.getAsJsonObject();
			Address a = toAddr(l.get("addr").getAsLong());
			String full = l.get("name").getAsString();
			int sep = full.lastIndexOf("::");
			Namespace ns = getClassNs(full.substring(0, sep));
			String name = full.substring(sep + 2);
			if (l.get("func").getAsBoolean()) {
				Function fn = getFunctionAt(a);
				if (fn == null) {
					fn = createFunction(a, null);
				}
				if (fn != null) {
					if (fn.getSymbol().getSource() != SourceType.IMPORTED) {
						fn.setParentNamespace(ns);
						fn.setName(name, SourceType.ANALYSIS);
					}
					funcs++;
					continue;
				}
			}
			currentProgram.getSymbolTable().createLabel(a, name, ns, SourceType.ANALYSIS);
		}

		monitor.setMessage("vtables");
		PointerDataType ptr = new PointerDataType(VoidDataType.dataType, 8, dtm);
		for (JsonElement e : root.getAsJsonArray("vtables")) {
			JsonObject v = e.getAsJsonObject();
			Address a = toAddr(v.get("vptr").getAsLong());
			int n = v.get("slots").getAsInt();
			try {
				clearListing(a, a.add(n * 8L - 1));
				createData(a, new ArrayDataType(ptr, n, 8, dtm));
			}
			catch (Exception ex) {
				printerr("vtable " + v.get("class").getAsString() + ": " + ex.getMessage());
			}
		}
		println("structs: " + structs.size() + ", labels: " + labels.size() + ", functions: " + funcs);
	}

	private Namespace getClassNs(String name) throws Exception {
		Namespace ns = classNs.get(name);
		if (ns == null) {
			SymbolTable st = currentProgram.getSymbolTable();
			ns = st.getNamespace(name, currentProgram.getGlobalNamespace());
			if (ns == null) {
				try {
					ns = st.createClass(currentProgram.getGlobalNamespace(), name, SourceType.ANALYSIS);
				}
				catch (DuplicateNameException ex) {
					ns = st.getNamespace(name, currentProgram.getGlobalNamespace());
				}
			}
			classNs.put(name, ns);
		}
		return ns;
	}

	private DataType parseType(String c, int len) {
		int br = c.indexOf('[');
		if (br >= 0) {
			int n = Integer.parseInt(c.substring(br + 1, c.indexOf(']')));
			DataType el = parseType(c.substring(0, br), -1);
			return new ArrayDataType(el, n, el.getLength(), dtm);
		}
		switch (c) {
			case "bool": return BooleanDataType.dataType;
			case "int8_t": return SignedByteDataType.dataType;
			case "uint8_t": return ByteDataType.dataType;
			case "int16_t": return ShortDataType.dataType;
			case "uint16_t": return UnsignedShortDataType.dataType;
			case "int32_t": return IntegerDataType.dataType;
			case "uint32_t": return UnsignedIntegerDataType.dataType;
			case "int64_t": return LongLongDataType.dataType;
			case "uint64_t": return UnsignedLongLongDataType.dataType;
			case "float": return FloatDataType.dataType;
			case "double": return DoubleDataType.dataType;
			case "char*": return new PointerDataType(CharDataType.dataType, 8, dtm);
			case "void*":
			case "igHandle": return new PointerDataType(VoidDataType.dataType, 8, dtm);
			default: return new ArrayDataType(ByteDataType.dataType, Math.max(len, 1), 1, dtm);
		}
	}
}
