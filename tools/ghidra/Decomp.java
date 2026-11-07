// Decompile the functions containing the addresses listed in <in-file>, write C + xrefs to <out-file>.
// Args: <in-file> <out-file>. Lines: "0xADDR" (decompile), "x 0xADDR" (xrefs only), "d 0xADDR n" (disasm n insns),
// "s NAME" (symbols with that name), "r 0xLO 0xHI" (symbols in [LO, HI)), "t TEXT" (string occurrences + their refs)
//@category GTASA
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class Decomp extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(new File(args[1]), "UTF-8")) {
			for (String line : Files.readAllLines(new File(args[0]).toPath())) {
				line = line.trim();
				if (line.isEmpty() || line.startsWith("#")) continue;
				String[] p = line.split("\s+");
				if (p[0].equals("x")) {
					Address a = toAddr(Long.decode(p[1]));
					w.println("== xrefs to " + a);
					for (Reference r : getReferencesTo(a)) {
						Function rf = getFunctionContaining(r.getFromAddress());
						Instruction src = getInstructionAt(r.getFromAddress());
						w.println("   " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (rf == null ? "-" : rf.getName(true) + "@" + rf.getEntryPoint())
							+ (src == null ? "" : "  [" + src + "]"));
					}
					continue;
				}
				if (p[0].equals("t")) {
					String text = line.substring(2);
					w.println("== string \"" + text + "\"");
					Address at = currentProgram.getMinAddress();
					byte[] pat = text.getBytes("ISO-8859-1");
					int found = 0;
					while (at != null && found < 8) {
						at = currentProgram.getMemory().findBytes(at, pat, null, true, monitor);
						if (at == null) break;
						found++;
						w.println("   at " + at);
						for (Reference r : getReferencesTo(at)) {
							Function rf = getFunctionContaining(r.getFromAddress());
							w.println("      ref " + r.getFromAddress() + " in " + (rf == null ? "-" : rf.getName(true) + "@" + rf.getEntryPoint()));
						}
						at = at.add(1);
					}
					continue;
				}
				if (p[0].equals("s")) {
					w.println("== symbols named " + p[1]);
					for (Symbol s : currentProgram.getSymbolTable().getSymbols(p[1]))
						w.println("   " + s.getAddress() + " " + s.getName(true));
					continue;
				}
				if (p[0].equals("r")) {
					Address lo = toAddr(Long.decode(p[1])), hi = toAddr(Long.decode(p[2]));
					w.println("== symbols in " + lo + ".." + hi);
					SymbolIterator it = currentProgram.getSymbolTable().getSymbolIterator(lo, true);
					while (it.hasNext()) {
						Symbol s = it.next();
						if (s.getAddress().compareTo(hi) >= 0) break;
						w.println("   " + s.getAddress() + " " + s.getName(true));
					}
					continue;
				}
				if (p[0].equals("d")) {
					Address a = toAddr(Long.decode(p[1]));
					int n = Integer.parseInt(p[2]);
					w.println("== disasm " + a);
					Instruction ins = getInstructionAt(a);
					if (ins == null) { disassemble(a); ins = getInstructionAt(a); }
					for (int i = 0; i < n && ins != null; i++, ins = ins.getNext())
						w.println("   " + ins.getAddress() + "  " + ins);
					continue;
				}
				Address a = toAddr(Long.decode(p[0]));
				Function f = getFunctionContaining(a);
				if (f == null) { w.println("== " + a + ": no function"); continue; }
				w.println("== " + a + " in " + f.getName(true) + " @" + f.getEntryPoint());
				DecompileResults res = di.decompileFunction(f, 120, monitor);
				if (res.decompileCompleted()) w.println(res.getDecompiledFunction().getC());
				else w.println("   decompile failed: " + res.getErrorMessage());
			}
		}
		di.dispose();
	}
}
