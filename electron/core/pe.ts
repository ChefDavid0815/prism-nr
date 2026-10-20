import * as fs from 'node:fs';
import type { Bitness } from '../shared/types';

export interface PeInfo {
  bitness: Bitness;
  imports: string[];
  isGui: boolean;
}

function readAt(fd: number, offset: number, length: number): Buffer {
  const buf = Buffer.alloc(length);
  const n = fs.readSync(fd, buf, 0, length, offset);
  return n === length ? buf : buf.subarray(0, n);
}

function readCString(fd: number, offset: number, max = 256): string {
  const buf = readAt(fd, offset, max);
  const end = buf.indexOf(0);
  return buf.subarray(0, end < 0 ? buf.length : end).toString('latin1');
}

/**
 * Reads the machine type and the import table of a PE file without loading it whole.
 * Returns null for files that are not valid PE images.
 */
export function readPe(file: string): PeInfo | null {
  let fd: number;
  try {
    fd = fs.openSync(file, 'r');
  } catch {
    return null;
  }
  try {
    const dos = readAt(fd, 0, 64);
    if (dos.length < 64 || dos.readUInt16LE(0) !== 0x5a4d) return null;
    const peOff = dos.readUInt32LE(0x3c);
    const head = readAt(fd, peOff, 24 + 240);
    if (head.length < 24 || head.readUInt32LE(0) !== 0x00004550) return null;
    const machine = head.readUInt16LE(4);
    const numSections = head.readUInt16LE(6);
    const optSize = head.readUInt16LE(20);
    const optOff = peOff + 24;
    const opt = readAt(fd, optOff, optSize);
    const magic = opt.readUInt16LE(0);
    const is64 = magic === 0x20b;
    const subsystem = opt.readUInt16LE(68);
    const ddBase = is64 ? 112 : 96;
    const importRva = opt.length >= ddBase + 16 ? opt.readUInt32LE(ddBase + 8) : 0;
    // Data directory 13: delay-load imports. Unreal and many engines delay-load d3d12.dll.
    const delayRva = opt.length >= ddBase + 13 * 8 + 8 ? opt.readUInt32LE(ddBase + 13 * 8) : 0;

    const secOff = optOff + optSize;
    const secs = readAt(fd, secOff, numSections * 40);
    const sections: { va: number; vsize: number; raw: number; rawSize: number }[] = [];
    for (let i = 0; i < numSections; i++) {
      const b = i * 40;
      if (b + 40 > secs.length) break;
      sections.push({ vsize: secs.readUInt32LE(b + 8), va: secs.readUInt32LE(b + 12), rawSize: secs.readUInt32LE(b + 16), raw: secs.readUInt32LE(b + 20) });
    }
    const rvaToOff = (rva: number): number | null => {
      for (const s of sections) {
        const size = Math.max(s.vsize, s.rawSize);
        if (rva >= s.va && rva < s.va + size) return rva - s.va + s.raw;
      }
      return null;
    };

    const imports: string[] = [];
    const impOff = importRva ? rvaToOff(importRva) : null;
    if (impOff !== null) {
      for (let i = 0; i < 512; i++) {
        const desc = readAt(fd, impOff + i * 20, 20);
        if (desc.length < 20) break;
        const nameRva = desc.readUInt32LE(12);
        if (nameRva === 0 && desc.readUInt32LE(0) === 0 && desc.readUInt32LE(16) === 0) break;
        const nameOff = rvaToOff(nameRva);
        if (nameOff !== null) imports.push(readCString(fd, nameOff).toLowerCase());
      }
    }
    const delayOff = delayRva ? rvaToOff(delayRva) : null;
    if (delayOff !== null) {
      for (let i = 0; i < 512; i++) {
        const desc = readAt(fd, delayOff + i * 32, 32);
        if (desc.length < 32) break;
        const attrs = desc.readUInt32LE(0);
        const nameRva = desc.readUInt32LE(4);
        if (nameRva === 0) break;
        // Old-style descriptors (attrs bit 0 clear) store VAs instead of RVAs; skip those.
        if ((attrs & 1) === 0) continue;
        const nameOff = rvaToOff(nameRva);
        if (nameOff !== null) imports.push(readCString(fd, nameOff).toLowerCase());
      }
    }
    const bitness: Bitness = machine === 0x8664 || machine === 0xaa64 ? 'x64' : machine === 0x14c ? 'x86' : is64 ? 'x64' : 'unknown';
    return { bitness, imports, isGui: subsystem === 2 };
  } catch {
    return null;
  } finally {
    fs.closeSync(fd);
  }
}
