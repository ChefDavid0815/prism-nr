// Generates Prism Core's loader proxies (dxgi.dll, d3d12.dll, winmm.dll, dinput8.dll): for each, the
// export list read from the system's own DLL, a .def that exports every name (and ordinal) as a
// thunk, and the thunks themselves (x64 GAS): each jumps through a table that src/proxy/proxy.cpp
// fills from the real system DLL at load, so every export keeps its exact signature (floats, any
// number of arguments) and its behaviour.
// Usage: node tools/proxy/gen.cjs   (writes build/proxy/<name>.def and <name>_thunks.S)
const fs = require('fs');
const path = require('path');

const NAMES = ['dxgi', 'd3d12', 'winmm', 'dinput8'];
const sys = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32');
const out = path.join(__dirname, '..', '..', 'build', 'proxy');
fs.mkdirSync(out, { recursive: true });

function exportsOf(file) {
  const b = fs.readFileSync(file);
  const pe = b.readUInt32LE(0x3c);
  const nsec = b.readUInt16LE(pe + 6), optSize = b.readUInt16LE(pe + 20), opt = pe + 24;
  const is64 = b.readUInt16LE(opt) === 0x20b;
  const dd = opt + (is64 ? 112 : 96);
  const expRva = b.readUInt32LE(dd), expSize = b.readUInt32LE(dd + 4);
  const secs = [];
  for (let i = 0; i < nsec; i++) { const s = opt + optSize + i * 40; secs.push({ va: b.readUInt32LE(s + 12), vs: Math.max(b.readUInt32LE(s + 8), b.readUInt32LE(s + 16)), raw: b.readUInt32LE(s + 20) }); }
  const off = (rva) => { for (const s of secs) if (rva >= s.va && rva < s.va + s.vs) return rva - s.va + s.raw; throw new Error('rva ' + rva); };
  const e = off(expRva);
  const base = b.readUInt32LE(e + 16), nfun = b.readUInt32LE(e + 20), nname = b.readUInt32LE(e + 24);
  const funs = off(b.readUInt32LE(e + 28)), names = off(b.readUInt32LE(e + 32)), ords = off(b.readUInt32LE(e + 36));
  const byIndex = new Map();
  for (let i = 0; i < nname; i++) {
    const nOff = off(b.readUInt32LE(names + i * 4));
    let end = nOff; while (b[end]) end++;
    byIndex.set(b.readUInt16LE(ords + i * 2), b.toString('latin1', nOff, end));
  }
  const list = [];
  for (let i = 0; i < nfun; i++) {
    const rva = b.readUInt32LE(funs + i * 4);
    if (!rva) continue;
    list.push({ ordinal: base + i, name: byIndex.get(i) || null, forwarded: rva >= expRva && rva < expRva + expSize });
  }
  return list;
}

for (const n of NAMES) {
  const list = exportsOf(path.join(sys, n + '.dll'));
  const def = [`LIBRARY ${n}`, 'EXPORTS'];
  const asm = ['    .text'];
  const table = [];
  list.forEach((x, i) => {
    def.push(x.name ? `    ${x.name}=PrismThunk${i} @${x.ordinal}` : `    PrismOrd${x.ordinal}=PrismThunk${i} @${x.ordinal} NONAME`);
    asm.push(`    .globl PrismThunk${i}`, `PrismThunk${i}:`, `    jmp *PrismReal+${i * 8}(%rip)`);
    table.push(x.name ? `{"${x.name}", 0}` : `{nullptr, ${x.ordinal}}`);
  });
  asm.push('    .data', '    .globl PrismReal', '    .p2align 3', 'PrismReal:', `    .fill ${list.length}, 8, 0`);
  fs.writeFileSync(path.join(out, `${n}.def`), def.join('\n') + '\n');
  fs.writeFileSync(path.join(out, `${n}_thunks.S`), asm.join('\n') + '\n');
  fs.writeFileSync(path.join(out, `${n}_table.inc`), `// ${n}.dll exports, in thunk order (name, or ordinal when unnamed)\n` + table.join(',\n') + '\n');
  console.log(`${n}: ${list.length} exports (${list.filter((x) => !x.name).length} by ordinal only)`);
}
