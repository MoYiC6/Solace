"""Pure parsing helpers for Solace's original XOR-encoded address records."""
import struct
import pefile


def address_records(data):
    pe = pefile.PE(data=bytes(data), fast_load=True)
    sections = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'_addr']
    if len(sections) != 1:
        raise ValueError('Expected exactly one _addr section')
    section = sections[0]
    if section.Misc_VirtualSize % 16:
        raise ValueError('Unexpected address record alignment')
    records = {}
    for delta in range(0, section.Misc_VirtualSize, 16):
        kind, ptr = struct.unpack_from('<QQ', data, section.PointerToRawData+delta)
        if kind == ptr == 0:
            continue
        offset = pe.get_offset_from_rva(ptr-pe.OPTIONAL_HEADER.ImageBase)
        end = data.find(b'\xAA', offset, offset+4096)
        if end < 0:
            raise ValueError('Missing XOR-encoded string terminator')
        records[delta//16] = (kind, ptr, bytes(v ^ 0xAA for v in data[offset:end]))
    return records
