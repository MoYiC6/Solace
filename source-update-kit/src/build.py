"""Reproducible offline Solace entity-bootstrap patch builder.

Requires the original DLL. This is not a reimplementation of the whole DLL.
All version-specific addresses and input hashes are explicit in the profile.
No game process is opened and no DLL is loaded.
"""
import argparse
import datetime
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import struct

import capstone
import keystone
import pefile
from pe_codec import address_records


def integer(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(n, a):
    return (n+a-1)//a*a


def assemble(source, profile):
    code_rva = integer(profile['layout']['code_rva'])
    state_rva = integer(profile['layout']['state_rva'])
    symbols = {name:integer(value) for name,value in profile['symbols'].items()}
    symbols.update({'state_'+name:state_rva+integer(offset) for name,offset in profile['state_layout'].items()})
    parameters = {name:integer(value) for name,value in profile['parameters'].items()}
    fixups = {}

    def substitute(match):
        kind, name = match.groups()
        if kind == 'value':
            return hex(parameters[name])
        if kind == 'rva':
            return hex(symbols[name])
        token = 0x11000000 + len(fixups)*0x100
        fixups[token] = symbols[name]
        return f'[rip + {token:#x}]'

    # Comments are explanatory source, not passed to Keystone's assembler.
    template = '\n'.join(line.split(';', 1)[0] for line in source.splitlines())
    rendered = re.sub(r'@(mem|rva|value):([A-Za-z_][A-Za-z_0-9]*)@', substitute, template)
    if '@' in rendered:
        raise ValueError('Unknown assembly macro; use @mem:name@, @rva:name@ or @value:name@')
    engine = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)
    code = bytearray(engine.asm(rendered, addr=code_rva, as_bytes=True)[0])
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    references = []
    for ins in dis.disasm(code, code_rva):
        for operand in ins.operands:
            if operand.type == capstone.x86.X86_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP:
                if operand.mem.disp not in fixups or ins.disp_size != 4:
                    raise ValueError('Unexpected RIP operand; global accesses must use named macros')
                target = fixups.pop(operand.mem.disp)
                struct.pack_into('<i', code, ins.address-code_rva+ins.disp_offset, target-ins.address-ins.size)
                references.append({'instruction_rva':hex(ins.address), 'target_rva':hex(target)})
    if fixups:
        raise ValueError('Assembly has unresolved RIP references')
    instructions = list(dis.disasm(code, code_rva))
    if sum(i.size for i in instructions) != len(code):
        raise ValueError('Could not disassemble the entire generated function')
    prefix = bytes.fromhex(profile['unwind']['prologue_prefix_hex'])
    if not code.startswith(prefix):
        raise ValueError('Prologue changed: update and independently validate its unwind metadata')
    unwind = bytes.fromhex(profile['unwind']['unwind_info_hex'])
    if len(unwind) < 4 or unwind[0] != 1 or unwind[1] != len(prefix):
        raise ValueError('Unsupported or inconsistent unwind header')
    # Original candidate ABI: four pushes then a 0x28 stack allocation.
    # Any intentional ABI change requires revising this check and its tests.
    if prefix != bytes.fromhex('53565741544883ec28') or unwind != bytes.fromhex('01090500094205c00370026001300000'):
        raise ValueError('This builder validates only the reviewed four-push/0x28 ABI')
    listing = '\n'.join(f'{i.address:08X}  {i.bytes.hex():<32} {i.mnemonic} {i.op_str}' for i in instructions)+'\n'
    return bytes(code), unwind, listing, references


def changed_spans(before, after):
    spans, start = [], None
    for i, (a, b) in enumerate(zip(before, after)):
        if a != b and start is None:
            start = i
        elif a == b and start is not None:
            spans.append({'file_offset':hex(start), 'old_hex':before[start:i].hex(), 'new_hex':after[start:i].hex()})
            start = None
    if start is not None:
        spans.append({'file_offset':hex(start), 'old_hex':before[start:].hex(), 'new_hex':after[start:len(before)].hex()})
    return spans


def build(original, client, profile, assembly):
    if sha(original) != profile['source_sha256']:
        raise ValueError('Input is not the original DLL pinned by this profile')
    if sha(client) != profile['client_sha256']:
        raise ValueError('Game build changed. Recheck addresses/ABI before creating a new profile; changing the hash alone does not establish compatibility.')
    layout = profile['layout']
    code_rva, state_rva = integer(layout['code_rva']), integer(layout['state_rva'])
    pe = pefile.PE(data=original, fast_load=False)
    if (pe.FILE_HEADER.NumberOfSections, pe.OPTIONAL_HEADER.SizeOfImage, len(original), pe.OPTIONAL_HEADER.ImageBase) != (
        integer(layout['original_section_count']), code_rva, integer(layout['original_size']), integer(layout['image_base'])):
        raise ValueError('Unexpected input PE layout')
    file_align, section_align = pe.OPTIONAL_HEADER.FileAlignment, pe.OPTIONAL_HEADER.SectionAlignment
    if (file_align, section_align) != (0x200, 0x1000) or state_rva != code_rva+section_align:
        raise ValueError('Unreviewed section/file alignment or layout')
    callback = integer(layout['callback_instruction_rva'])
    expected = bytes.fromhex(layout['callback_original_hex'])
    if pe.get_data(callback, len(expected)) != expected:
        raise ValueError('FrameStage callback instruction anchor does not match')
    code, unwind, listing, references = assemble(assembly, profile)
    unwind_offset = align(len(code), 4)
    rx_data = code + bytes(unwind_offset-len(code)) + unwind
    rx_size = align(len(rx_data), file_align)
    if rx_size > section_align:
        raise ValueError('New function is too large for this reviewed layout')
    rw_data = bytearray(file_align)
    marker_offset = integer(profile['state_layout']['marker'])
    marker = profile['state_marker'].encode('ascii') + b'\0'
    if marker_offset+len(marker) > len(rw_data):
        raise ValueError('Diagnostic marker exceeds state section')
    rw_data[marker_offset:marker_offset+len(marker)] = marker
    result = bytearray(original) + rx_data + bytes(rx_size-len(rx_data)) + rw_data

    def put32(offset, value):
        struct.pack_into('<I', result, offset, value)

    header_end = pe.sections[-1].get_file_offset()+40
    first_raw = min(s.PointerToRawData for s in pe.sections if s.SizeOfRawData)
    if header_end+80 > first_raw or any(original[header_end:header_end+80]):
        raise ValueError('No unoccupied space for two additional section headers')
    for offset, name, vsize, rva, raw_size, raw_offset, flags in [
        (header_end,b'.sinit',len(rx_data),code_rva,rx_size,len(original),0x60000020),
        (header_end+40,b'.sstate',len(rw_data),state_rva,len(rw_data),len(original)+rx_size,0xC0000040),
    ]:
        struct.pack_into('<8sIIIIIIHHI',result,offset,name,vsize,rva,raw_size,raw_offset,0,0,0,0,flags)
    struct.pack_into('<H',result,pe.FILE_HEADER.get_field_absolute_offset('NumberOfSections'),pe.FILE_HEADER.NumberOfSections+2)
    put32(pe.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfImage'),state_rva+section_align)
    put32(pe.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfCode'),pe.OPTIONAL_HEADER.SizeOfCode+rx_size)
    put32(pe.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfInitializedData'),pe.OPTIONAL_HEADER.SizeOfInitializedData+len(rw_data))
    displacement_rva = callback+integer(layout['callback_displacement_offset'])
    displacement_offset = pe.get_offset_from_rva(displacement_rva)
    struct.pack_into('<i',result,displacement_offset,code_rva-callback-integer(layout['callback_instruction_length']))
    exception = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    pdata = pe.get_section_by_rva(exception.VirtualAddress)
    table_end = exception.VirtualAddress+exception.Size
    if table_end+12 > pdata.VirtualAddress+pdata.SizeOfRawData:
        raise ValueError('Exception table has insufficient trailing raw space')
    record_offset = pe.get_offset_from_rva(table_end)
    if any(original[record_offset:record_offset+12]):
        raise ValueError('Exception table padding is occupied')
    struct.pack_into('<III',result,record_offset,code_rva,code_rva+len(code),code_rva+unwind_offset)
    put32(exception.get_field_absolute_offset('Size'),exception.Size+12)
    put32(pdata.get_field_absolute_offset('Misc_VirtualSize'),pdata.Misc_VirtualSize+12)
    checksum_offset = pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum')
    put32(checksum_offset,0)
    put32(checksum_offset,pefile.PE(data=bytes(result),fast_load=True).generate_checksum())
    check = pefile.PE(data=bytes(result),fast_load=False)
    if not check.verify_checksum():
        raise ValueError('Resulting PE checksum failed')
    old_entries = [(e.struct.BeginAddress,e.struct.EndAddress,e.struct.UnwindData) for e in pe.DIRECTORY_ENTRY_EXCEPTION]
    new_entries = [(e.struct.BeginAddress,e.struct.EndAddress,e.struct.UnwindData) for e in check.DIRECTORY_ENTRY_EXCEPTION]
    if new_entries != old_entries+[(code_rva,code_rva+len(code),code_rva+unwind_offset)]:
        raise ValueError('Exception directory changed outside the new entry')
    if [s.VirtualAddress for s in check.sections] != [s.VirtualAddress for s in pe.sections]+[code_rva,state_rva]:
        raise ValueError('An original section address changed')
    for index in range(16):
        if index == 3:
            continue
        before, after = pe.OPTIONAL_HEADER.DATA_DIRECTORY[index],check.OPTIONAL_HEADER.DATA_DIRECTORY[index]
        if (before.VirtualAddress,before.Size) != (after.VirtualAddress,after.Size):
            raise ValueError(f'Unrelated data directory {index} changed')
    text_section = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    for i in range(text_section.PointerToRawData,text_section.PointerToRawData+text_section.SizeOfRawData):
        if result[i] != original[i] and not displacement_offset <= i < displacement_offset+4:
            raise ValueError('Unexpected modification to original executable code')
    records = address_records(original)
    if len(records) != integer(layout['address_record_count']) or records != address_records(result):
        raise ValueError('Original address patterns changed')
    info = {'wrapper_rva':hex(code_rva),'wrapper_size':len(code),'unwind_rva':hex(code_rva+unwind_offset),
            'unwind_hex':unwind.hex(),'state_rva':hex(state_rva),'symbols':profile['symbols'],
            'state_layout':profile['state_layout'],'statuses':profile['status_meanings'],
            'original_file_changed_spans':changed_spans(original,result),'rip_fixups':references,
            'unchanged_address_pattern_count':len(records)}
    return bytes(result),listing,info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',required=True,type=Path,help='Original Solace.dll')
    parser.add_argument('--client',required=True,type=Path,help='Current game client.dll')
    parser.add_argument('--profile',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args = parser.parse_args()
    source_path,client_path,profile_path,output = (p.resolve() for p in (args.input,args.client,args.profile,args.output))
    if output in (source_path,client_path,profile_path):
        raise ValueError('Output must be a separate file')
    profile_bytes = profile_path.read_bytes()
    profile = json.loads(profile_bytes)
    assembly_path = (profile_path.parent/profile['assembly']).resolve()
    assembly_bytes = assembly_path.read_bytes()
    original, client = source_path.read_bytes(),client_path.read_bytes()
    result,listing,info = build(original,client,profile,assembly_bytes.decode('utf-8'))
    if output.exists() and output.read_bytes() != result:
        raise FileExistsError('Output differs from an existing candidate. Choose a new --output path to preserve it.')
    output.parent.mkdir(parents=True,exist_ok=True)
    if not output.exists():
        with output.open('xb') as stream:
            stream.write(result)
    output.with_suffix('.disasm.txt').write_text(listing,encoding='utf-8')
    manifest = {
        'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'status':'structurally-validated-runtime-unverified',
        'scope':profile['scope'],'source':str(source_path),'source_sha256':sha(original),
        'client':str(client_path),'client_sha256':sha(client),'game_build':profile['game_build'],
        'output':str(output),'output_sha256':sha(result),'size':len(result),
        'profile':str(profile_path),'profile_sha256':sha(profile_bytes),
        'assembly':str(assembly_path),'assembly_sha256':sha(assembly_bytes),
        'dependency_distribution_versions':{name:importlib.metadata.version(name) for name in ['pefile','capstone','keystone-engine']},
        'dependency_module_versions':{'capstone':capstone.__version__,'keystone':keystone.__version__,'pefile':pefile.__version__},
        'limitations':['Requires the original DLL; not the complete source of Solace.',
                       'Does not include the separate draw_legs pattern patch.',
                       'No game process was opened and this build does not establish game-runtime correctness.'],
        **info,
    }
    manifest_path = output.with_suffix('.manifest.json')
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    if source_path.read_bytes() != original or output.read_bytes() != result:
        raise ValueError('Post-write byte verification failed')
    print(json.dumps({'output':str(output),'sha256':sha(result),'manifest':str(manifest_path),'status':manifest['status']},ensure_ascii=False))


if __name__ == '__main__':
    main()
