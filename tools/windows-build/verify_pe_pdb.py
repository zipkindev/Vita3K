#!/usr/bin/env python3
"""Validate native MSF7 PDB GUID/age against a PE CodeView RSDS record."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid

def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def pe_identity(data):
    pe = u32(data, 0x3c)
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    sections, opt_size = struct.unpack_from('<H12xH', data, pe+6)
    opt = pe+24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic not in (0x10b, 0x20b):
        raise ValueError('Unsupported PE optional header')
    dirs = opt+(112 if magic == 0x20b else 96)
    debug_rva, debug_size = struct.unpack_from('<II', data, dirs+6*8)
    def rva_offset(rva):
        for i in range(sections):
            base = opt+opt_size+i*40
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from('<IIII', data, base+8)
            if virtual_address <= rva < virtual_address+max(virtual_size, raw_size):
                return raw_offset+rva-virtual_address
        raise ValueError('Debug directory outside PE sections')
    debug = rva_offset(debug_rva)
    for pos in range(debug, debug+debug_size, 28):
        kind, size, _, pointer = struct.unpack_from('<IIII', data, pos+12)
        cv = data[pointer:pointer+size]
        if kind == 2 and cv[:4] == b'RSDS':
            return {'guid': str(uuid.UUID(bytes_le=bytes(cv[4:20]))), 'age': u32(cv, 20), 'pdbPath': cv[24:].split(b'\0')[0].decode('utf-8', errors='replace')}
    raise ValueError('No RSDS PDB record in EXE')

def pdb_identity(data):
    if data[:32] != b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0':
        raise ValueError('Not an MSF7 PDB')
    block_size, _, _, size, _, map_block = struct.unpack_from('<6I', data, 32)
    count = (size+block_size-1)//block_size
    blocks = struct.unpack_from('<'+str(count)+'I', data, map_block*block_size)
    directory = b''.join(data[b*block_size:(b+1)*block_size] for b in blocks)[:size]
    streams = u32(directory, 0)
    sizes = struct.unpack_from('<'+str(streams)+'I', directory, 4)
    cursor = 4+4*streams
    info = None
    for i, stream_size in enumerate(sizes):
        if stream_size == 0xffffffff:
            continue
        n = (stream_size+block_size-1)//block_size
        indices = struct.unpack_from('<'+str(n)+'I', directory, cursor)
        cursor += 4*n
        if i == 1:
            info = b''.join(data[b*block_size:(b+1)*block_size] for b in indices)[:stream_size]
            break
    if info is None or len(info) < 28:
        raise ValueError('Missing PDB info stream')
    return {'guid': str(uuid.UUID(bytes_le=bytes(info[12:28]))), 'age': u32(info, 8)}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('exe', type=Path)
    p.add_argument('pdb', type=Path)
    args = p.parse_args()
    exe, pdb = args.exe.read_bytes(), args.pdb.read_bytes()
    ei, pi = pe_identity(exe), pdb_identity(pdb)
    matches = all(ei[key] == pi[key] for key in ('guid', 'age'))
    print(json.dumps({'matched': matches, 'exe': ei, 'pdb': pi, 'exeSha256': hashlib.sha256(exe).hexdigest(), 'pdbSha256': hashlib.sha256(pdb).hexdigest()}, indent=2))
    if not matches:
        raise SystemExit('EXE/PDB mismatch')

if __name__ == '__main__':
    main()
