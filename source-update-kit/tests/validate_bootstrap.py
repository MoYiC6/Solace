"""Independent semantic tests of a PE bootstrap wrapper using Unicorn x64.

All target instructions run in emulator memory. This script does not load the
DLL through Windows, open another process, or call any target/Windows API.
The supplied builder manifest provides addresses and diagnostic-state layout.
No workspace paths, private dependencies, or target process are required.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import pefile
import unicorn
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_PROT_ALL, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import *

STACK=0x0000000070000000
STACK_SIZE=0x20000
HEAP=0x0000000040000000
HEAP_SIZE=0x1000000
STUB=0x0000000060000000
RETURN=STUB+0xF00
ORIGINAL_RETURN=0x8877665544332211
ORIGINAL_XMM0=0xFFEEDDCCBBAA99887766554433221100
THIS_POINTER=HEAP+0x900000
SYSTEM=HEAP+0x10000
GLOBAL_SLOT=HEAP+0x9000
ENTITY_POOL=HEAP+0x600000
CACHE_STORAGE=HEAP+0x800000
CONTROLLER=ENTITY_POOL+0x100
PAWN=ENTITY_POOL+0x200

NONVOLATILE={
    'RBX':UC_X86_REG_RBX,'RBP':UC_X86_REG_RBP,'RSI':UC_X86_REG_RSI,'RDI':UC_X86_REG_RDI,
    'R12':UC_X86_REG_R12,'R13':UC_X86_REG_R13,'R14':UC_X86_REG_R14,'R15':UC_X86_REG_R15,
}
VOLATILE=[UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9,UC_X86_REG_R10,UC_X86_REG_R11]


def integer(value):
    return int(value,0) if isinstance(value,str) else value


def align(value):
    return (value+0xFFF)&~0xFFF


def specification(manifest):
    """Resolve the public manifest; reject missing/ambiguous contracts explicitly."""
    symbols=manifest['symbols']
    state_base=integer(manifest['state_rva'])
    layout={name:integer(value)for name,value in manifest['state_layout'].items()}
    if any(value<0 for value in layout.values()):
        raise ValueError('Diagnostic-state offsets must be nonnegative')
    descriptions={
        'waiting':'waiting-for-initial-ready-frame',
        'started':'initial-registration-started',
        'complete':'complete-local-controller-confirmed',
        'missing_controller':'failed-local-controller-missing',
    }
    statuses={}
    for name,description in descriptions.items():
        values=[integer(number)for number,text in manifest['statuses'].items()if text==description]
        if len(values)!=1:
            raise ValueError('Missing or ambiguous manifest status: '+description)
        statuses[name]=values[0]
    return {
        'candidate_sha256':manifest['output_sha256'],
        'wrapper_rva':integer(manifest['wrapper_rva']),
        'state_rva':state_base+layout['status'],
        'state_size':4,
        'entity_count_rva':state_base+layout['nonzero_entity_count'],
        'enumerated_limit_rva':state_base+layout['enumerated_index_limit'],
        'saved_controller_rva':state_base+layout['controller'],
        'statuses':statuses,
        'original_frame_stage_rva':integer(symbols['original_frame']),
        'on_add_rva':integer(symbols['entity_add']),
        'cache_rva':integer(symbols['entity_cache']),
        'entity_list_storage_rva':integer(symbols['entity_system_slot']),
        'loading_rva':integer(symbols['loading_gate']),
        'frame_ready_rva':integer(symbols['frame_ready']),
        'context_controller_rva':integer(symbols['controller']),
        'context_pawn_rva':integer(symbols['pawn']),
        'context_observer_rva':integer(symbols['observer_pawn']),
        'context_lock_rva':integer(symbols['context_lock']),
        'api_pointer_rvas':{
            'AcquireSRWLockShared':integer(symbols['acquire_shared_iat']),
            'ReleaseSRWLockShared':integer(symbols['release_shared_iat']),
        },
        'preserve_rax_return':True,
        'preserve_xmm0_return':False,
        'actual_unstubbed_functions':{
            'entity_lookup':integer(symbols['entity_lookup']),
            'entity_membership':integer(symbols['entity_membership']),
        },
    }


class Simulation:
    def __init__(self,dll,spec,image_base):
        self.spec=spec
        self.base=image_base
        self.uc=Uc(UC_ARCH_X86,UC_MODE_64)
        self.events=[]
        self.faults=[]
        self.calls=[]
        self.lock_depth=0
        self.lock_stack=[]
        self.local_cache={}
        self.reject_adds=False
        self.original_calls=0
        self.original_effects={}
        self.pe=pefile.PE(data=dll)
        imports={}
        for directory in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in directory.imports:
                if imp.name:
                    imports[imp.name.decode('ascii')]=imp.address-self.pe.OPTIONAL_HEADER.ImageBase
        self.uc.mem_map(image_base,align(self.pe.OPTIONAL_HEADER.SizeOfImage),UC_PROT_ALL)
        image=self.pe.get_memory_mapped_image(ImageBase=image_base)
        self.uc.mem_write(image_base,image)
        self.uc.mem_protect(image_base,0x1000,UC_PROT_READ)
        for section in self.pe.sections:
            permission=(UC_PROT_READ if section.Characteristics&0x40000000 else 0)
            permission|=UC_PROT_WRITE if section.Characteristics&0x80000000 else 0
            permission|=UC_PROT_EXEC if section.Characteristics&0x20000000 else 0
            if section.Misc_VirtualSize:
                self.uc.mem_protect(image_base+section.VirtualAddress,align(max(section.Misc_VirtualSize,section.SizeOfRawData)),permission)
        self.uc.mem_map(STACK,STACK_SIZE,UC_PROT_READ|UC_PROT_WRITE)
        self.uc.mem_map(HEAP,HEAP_SIZE,UC_PROT_READ|UC_PROT_WRITE)
        self.uc.mem_map(STUB,0x1000,UC_PROT_READ|UC_PROT_EXEC)
        self.uc.mem_write(STUB,b'\xC3'*0x1000)
        self.stubs={self.address('original_frame_stage_rva'):'original',self.address('on_add_rva'):'on_add'}
        for i,name in enumerate(['AcquireSRWLockShared','ReleaseSRWLockShared','AcquireSRWLockExclusive','ReleaseSRWLockExclusive']):
            if name in imports:
                address=STUB+0x100+i*0x20
                self.write64(image_base+imports[name],address)
                self.stubs[address]=name
        # Explicit IAT aliases may be supplied if the wrapper calls an existing
        # resolved API cache rather than the PE import address table.
        for name,rva in spec.get('api_pointer_rvas',{}).items():
            matches=[a for a,n in self.stubs.items() if n==name]
            if not matches:
                address=STUB+0x300+len(self.stubs)*0x20
                self.stubs[address]=name
            else:address=matches[0]
            self.write64(self.base+integer(rva),address)
        for address in self.stubs:
            self.uc.hook_add(UC_HOOK_CODE,self.stub,begin=address,end=address)
        self.uc.hook_add(UC_HOOK_MEM_INVALID,self.invalid)
        self.write64(GLOBAL_SLOT,SYSTEM)
        self.write64(self.address('entity_list_storage_rva'),GLOBAL_SLOT)
        self.write64(self.address('cache_rva'),CACHE_STORAGE)
        self.write64(self.address('cache_rva')+8,CACHE_STORAGE)
        self.write64(self.address('cache_rva')+16,CACHE_STORAGE+0x1000)
        self.reset_state()
        self.ready()

    def address(self,name):
        return self.base+integer(self.spec[name])

    def write64(self,address,value):
        self.uc.mem_write(address,struct.pack('<Q',value))

    def read64(self,address):
        return struct.unpack('<Q',self.uc.mem_read(address,8))[0]

    def reset_state(self):
        self.uc.mem_write(self.address('state_rva'),struct.pack('<I',self.status('waiting')))

    def status(self,name):
        return self.spec['statuses'][name]

    def state(self):
        return int.from_bytes(self.uc.mem_read(self.address('state_rva'),integer(self.spec.get('state_size',4))),'little')

    def ready(self):
        self.uc.mem_write(self.address('loading_rva'),b'\0')
        self.uc.mem_write(self.address('frame_ready_rva'),b'\1')
        self.write64(self.address('context_controller_rva'),CONTROLLER)
        self.write64(self.address('context_pawn_rva'),PAWN)
        self.write64(self.address('context_observer_rva'),0)
        for item in self.spec.get('ready_writes',[]):
            self.uc.mem_write(self.base+integer(item['rva']),struct.pack('<'+item['format'],integer(item['value'])))

    def set_slot(self,index,entity=None,serial=7,handle=None):
        chunk=index>>9
        chunk_address=HEAP+0x20000+chunk*0x10000
        self.write64(SYSTEM+0x10+chunk*8,chunk_address)
        identity=chunk_address+(index&0x1FF)*0x70
        entity=ENTITY_POOL+index*0x100 if entity is None else entity
        handle=((serial<<15)|index) if handle is None else handle
        self.write64(identity,entity)
        self.uc.mem_write(identity+0x10,struct.pack('<I',handle&0xffffffff))
        if entity:
            self.write64(entity+0x10,identity)
        # Existing on_add masks its 3rd argument to an entity index. The wrapper
        # deliberately passes the saved index, independent of serial bits.
        return entity,index

    def seed_cache(self,index,entity):
        self.local_cache[index]=entity
        self.sync_cache()

    def sync_cache(self):
        for i,(index,entity) in enumerate(sorted(self.local_cache.items())):
            self.uc.mem_write(CACHE_STORAGE+i*16,struct.pack('<QIhBB',entity,0,index,1,0))
        self.write64(self.address('cache_rva')+8,CACHE_STORAGE+len(self.local_cache)*16)

    def invalid(self,uc,access,address,size,value,user_data):
        self.faults.append({'access':access,'address':hex(address),'size':size,'value':hex(value)})
        return False

    def stub(self,uc,address,size,user_data):
        name=self.stubs[address]
        rsp=uc.reg_read(UC_X86_REG_RSP)
        args=[uc.reg_read(x)for x in [UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9]]
        event={'name':name,'rsp_mod_16':rsp%16,'args':[hex(x)for x in args]}
        self.events.append(event)
        if rsp%16!=8:raise AssertionError(f'{name}: Win64 callee stack alignment is {rsp%16}')
        return_address=self.read64(rsp)
        if name=='original':
            self.original_calls+=1
            if args[0]!=THIS_POINTER:raise AssertionError('Original RCX was not preserved')
            for key,value in self.original_effects.items():
                if key=='loading':self.uc.mem_write(self.address('loading_rva'),bytes([value]))
                elif key=='ready':self.uc.mem_write(self.address('frame_ready_rva'),bytes([value]))
        elif name=='on_add':
            if args[0]!=self.address('cache_rva'):raise AssertionError('on_add RCX does not identify the cache')
            if self.lock_depth:raise AssertionError('on_add called while the wrapper still holds the shared context lock')
            entity,handle=args[1],args[2]&0xffffffff
            self.calls.append((entity,handle))
            if not self.reject_adds:
                self.local_cache[handle&0x7fff]=entity
                self.sync_cache()
        elif name.startswith('AcquireSRWLock'):
            if args[0] not in (self.address('context_lock_rva'),self.address('cache_rva')+0x18):
                raise AssertionError('Unexpected SRW lock address')
            self.lock_depth+=1
            self.lock_stack.append(args[0])
        elif name.startswith('ReleaseSRWLock'):
            self.lock_depth-=1
            if self.lock_depth<0:raise AssertionError('Unbalanced SRW release')
            if not self.lock_stack or self.lock_stack.pop()!=args[0]:raise AssertionError('Mismatched SRW release address')
        else:raise AssertionError('Unexpected stub '+name)
        # Callees may use their caller-provided 32-byte shadow space and all
        # volatile registers. This makes preservation failures observable.
        uc.mem_write(rsp+8,b'\xA5'*32)
        for i,reg in enumerate(VOLATILE):uc.reg_write(reg,0xCCCC000000000000+i)
        for i in range(6):uc.reg_write(globals()['UC_X86_REG_XMM'+str(i)],0xDEADBEEF00000000+i)
        if name=='original':
            uc.reg_write(UC_X86_REG_RAX,ORIGINAL_RETURN)
            uc.reg_write(UC_X86_REG_XMM0,ORIGINAL_XMM0)
        else:uc.reg_write(UC_X86_REG_RAX,0)
        uc.reg_write(UC_X86_REG_RSP,rsp+8)
        uc.reg_write(UC_X86_REG_RIP,return_address)

    def invoke(self,stage):
        start_rsp=STACK+STACK_SIZE-0x108
        self.write64(start_rsp,RETURN)
        expected={}
        for i,(name,reg)in enumerate(NONVOLATILE.items()):
            value=0xABCDEF0000000000+i*0x100010001
            self.uc.reg_write(reg,value);expected[name]=value
        expected_xmm={}
        for i in range(6,16):
            value=(0xFEDCBA9876543210<<64)|i
            self.uc.reg_write(globals()['UC_X86_REG_XMM'+str(i)],value);expected_xmm[i]=value
        self.uc.reg_write(UC_X86_REG_RSP,start_rsp)
        self.uc.reg_write(UC_X86_REG_RCX,THIS_POINTER)
        self.uc.reg_write(UC_X86_REG_RDX,stage)
        self.uc.reg_write(UC_X86_REG_EFLAGS,0x202)
        before_calls=self.original_calls
        before_events=len(self.events)
        self.uc.emu_start(self.address('wrapper_rva'),RETURN,count=3000000)
        assert self.uc.reg_read(UC_X86_REG_RIP)==RETURN,'Wrapper did not return within the instruction budget'
        assert self.uc.reg_read(UC_X86_REG_RSP)==start_rsp+8,'Stack pointer was not restored'
        assert self.original_calls==before_calls+1,'Original FrameStage must run exactly once per wrapper call'
        original_event=next(e for e in self.events[before_events:]if e['name']=='original')
        assert int(original_event['args'][1],16)&0xffffffff==stage,'Original stage argument changed'
        assert self.lock_depth==0,'Unbalanced SRW lock'
        for name,reg in NONVOLATILE.items():assert self.uc.reg_read(reg)==expected[name],name+' changed'
        for i,value in expected_xmm.items():assert self.uc.reg_read(globals()['UC_X86_REG_XMM'+str(i)])==value,'XMM'+str(i)+' changed'
        if self.spec.get('preserve_rax_return',True):assert self.uc.reg_read(UC_X86_REG_RAX)==ORIGINAL_RETURN,'Original RAX return value changed'
        if self.spec.get('preserve_xmm0_return',False):assert self.uc.reg_read(UC_X86_REG_XMM0)==ORIGINAL_XMM0,'Original XMM0 return value changed'
        return {'stage':stage,'state':self.state(),'original_calls':self.original_calls,
                'on_add_calls':len(self.calls),'rax':hex(self.uc.reg_read(UC_X86_REG_RAX)),
                'nonvolatile_gpr_preserved':True,'nonvolatile_xmm_preserved':True,
                'stack_restored':True,'callee_alignment_checked':True}


def scenarios(dll,spec,image_base):
    results=[]
    def run(name,body):
        sim=Simulation(dll,spec,image_base)
        row={'name':name,'image_base':hex(image_base),'status':'failed'}
        try:
            row['observations']=body(sim)
            row['status']='passed'
        except Exception as error:
            row['error']=str(error)
        row['events']=sim.events
        row['on_add_calls']=[{'entity':hex(e),'handle':hex(h),'index':h&0x7fff}for e,h in sim.calls]
        row['memory_faults']=sim.faults
        results.append(row)

    def no_stage(sim):
        sim.set_slot(1)
        out=[sim.invoke(stage)for stage in (0,3,11,13)]
        assert not sim.calls and sim.state()==sim.status('waiting'),'Non-12 stage must not consume bootstrap'
        return out
    run('non_stage12_waits',no_stage)

    def loading(sim):
        expected=sim.set_slot(1)
        sim.uc.mem_write(sim.address('loading_rva'),b'\1')
        first=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting'),'Loading consumed bootstrap'
        sim.uc.mem_write(sim.address('loading_rva'),b'\0')
        second=sim.invoke(12)
        assert sim.calls==[expected] and sim.state()!=sim.status('waiting')
        return [first,second]
    run('loading_waits_then_ready',loading)

    def invalid_context(sim):
        expected=sim.set_slot(1)
        sim.write64(sim.address('context_controller_rva'),0)
        sim.write64(sim.address('context_pawn_rva'),0)
        sim.write64(sim.address('context_observer_rva'),0)
        first=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting'),'Invalid context consumed bootstrap'
        sim.ready()
        second=sim.invoke(12)
        assert sim.calls==[expected] and sim.state()!=sim.status('waiting')
        return [first,second]
    run('invalid_context_waits_then_ready',invalid_context)

    def not_ready(sim):
        expected=sim.set_slot(1)
        sim.uc.mem_write(sim.address('frame_ready_rva'),b'\0')
        first=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting'),'Frame-not-ready consumed bootstrap'
        sim.uc.mem_write(sim.address('frame_ready_rva'),b'\1')
        second=sim.invoke(12)
        assert sim.calls==[expected]
        return [first,second]
    run('frame_ready_gate',not_ready)

    def missing_system(sim):
        expected=sim.set_slot(1)
        sim.write64(GLOBAL_SLOT,0)
        first=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting'),'Null entity system consumed bootstrap'
        sim.write64(GLOBAL_SLOT,SYSTEM)
        second=sim.invoke(12)
        assert sim.calls==[expected]
        return [first,second]
    run('null_entity_system_waits',missing_system)

    def sparse(sim):
        indices=[1,7,511,512,513,1023,4095,16383]
        expected=[sim.set_slot(i,serial=3+i%7)for i in indices]
        sim.set_slot(9,entity=0)
        first=sim.invoke(12)
        assert sim.calls==expected,{'expected':expected,'actual':sim.calls}
        assert sim.state()!=sim.status('waiting')
        sim.set_slot(14)
        second=sim.invoke(12)
        assert sim.calls==expected,'A second call replayed the initial fill'
        return [first,second]
    run('sparse_table_chunk_boundaries_once',sparse)

    def nonempty(sim):
        expected=[sim.set_slot(i)for i in (1,3,512)]
        sim.seed_cache(1,expected[0][0])
        out=sim.invoke(12)
        assert sim.calls==expected,'Preexisting cache entries suppressed initial supplementation'
        assert set(sim.local_cache)=={1,3,512}
        return out
    run('nonempty_cache_still_bootstraps',nonempty)

    def original_first(sim):
        expected=sim.set_slot(1)
        sim.uc.mem_write(sim.address('frame_ready_rva'),b'\0')
        sim.original_effects={'ready':1}
        out=sim.invoke(12)
        assert sim.calls==[expected],'Wrapper did not observe state refreshed by the original handler'
        assert sim.events[0]['name']=='original'
        return out
    run('original_frame_stage_runs_before_ready_check',original_first)

    def observer_context(sim):
        expected=sim.set_slot(1)
        sim.write64(sim.address('context_pawn_rva'),0)
        sim.write64(sim.address('context_observer_rva'),PAWN)
        out=sim.invoke(12)
        assert sim.calls==[expected] and sim.state()==sim.status('complete')
        return out
    run('observer_can_supply_valid_context',observer_context)

    def missing_controller_after_scan(sim):
        expected=sim.set_slot(7)
        first=sim.invoke(12)
        assert sim.calls==[expected] and sim.state()==sim.status('missing_controller'),'Missing controller must be reported as terminal status 3'
        second=sim.invoke(12)
        assert sim.calls==[expected] and sim.state()==sim.status('missing_controller'),'Failure must not cause an unbounded replay'
        return [first,second]
    run('membership_false_nonempty_cache_is_status3',missing_controller_after_scan)

    def markers(sim):
        expected=[sim.set_slot(i)for i in (1,511,512,16383)]
        out=sim.invoke(12)
        status=sim.state()
        count=struct.unpack('<I',sim.uc.mem_read(sim.address('entity_count_rva'),4))[0]
        limit=struct.unpack('<I',sim.uc.mem_read(sim.address('enumerated_limit_rva'),4))[0]
        saved_controller=sim.read64(sim.address('saved_controller_rva'))
        assert status==sim.status('complete') and count==len(expected) and limit==0x4000 and saved_controller==CONTROLLER
        out['diagnostic_markers']={'status':status,'valid_entity_calls':count,'scanned_limit':limit,'controller':hex(saved_controller)}
        return out
    run('success_diagnostic_markers',markers)

    def missing_controller_only(sim):
        sim.set_slot(1)
        sim.write64(sim.address('context_controller_rva'),0)
        out=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting')
        return out
    run('controller_required_even_with_pawn',missing_controller_only)

    def missing_pawn_and_observer(sim):
        sim.set_slot(1)
        sim.write64(sim.address('context_pawn_rva'),0)
        sim.write64(sim.address('context_observer_rva'),0)
        out=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('waiting')
        return out
    run('pawn_or_observer_required_even_with_controller',missing_pawn_and_observer)

    def pointer_sentinels(sim):
        expected=sim.set_slot(1)
        observations=[]
        for outer,inner in [(0,SYSTEM),(0xffffffffffffffff,SYSTEM),(GLOBAL_SLOT,0),(GLOBAL_SLOT,0xffffffffffffffff)]:
            sim.write64(sim.address('entity_list_storage_rva'),outer)
            sim.write64(GLOBAL_SLOT,inner)
            observations.append(sim.invoke(12))
            assert not sim.calls and sim.state()==sim.status('waiting'),'Invalid entity-system pointer consumed bootstrap'
        sim.write64(sim.address('entity_list_storage_rva'),GLOBAL_SLOT)
        sim.write64(GLOBAL_SLOT,SYSTEM)
        observations.append(sim.invoke(12))
        assert sim.calls==[expected] and sim.state()==sim.status('complete')
        return observations
    run('null_and_minus_one_system_pointer_guards',pointer_sentinels)

    def range_boundaries(sim):
        sim.set_slot(0)
        expected=sim.set_slot(16383)
        sim.set_slot(16384)
        out=sim.invoke(12)
        assert sim.calls==[expected],'Index 0 or 0x4000 escaped the established query bounds'
        return out
    run('index0_and_0x4000_excluded',range_boundaries)

    def rejected_registration(sim):
        expected=[sim.set_slot(i)for i in (1,512)]
        sim.reject_adds=True
        first=sim.invoke(12)
        assert sim.calls==expected and sim.state()==sim.status('missing_controller')
        second=sim.invoke(12)
        assert sim.calls==expected and sim.state()==sim.status('missing_controller')
        return [first,second]
    run('on_add_rejection_reports_status3_once',rejected_registration)

    def busy_state(sim):
        sim.set_slot(1)
        sim.uc.mem_write(sim.address('state_rva'),struct.pack('<I',sim.status('started')))
        out=sim.invoke(12)
        assert not sim.calls and sim.state()==sim.status('started')
        return out
    run('in_progress_status_prevents_duplicate_start',busy_state)
    return results


def main():
    parser=argparse.ArgumentParser(description='Execute 34 offline bootstrap semantics/Win64 ABI cases in Unicorn.')
    parser.add_argument('--dll',type=Path,required=True,help='Candidate PE DLL; never loaded through the operating system')
    parser.add_argument('--manifest',type=Path,required=True,help='Builder JSON manifest with symbols and state layout')
    parser.add_argument('--output',type=Path,required=True,help='New JSON validation result path')
    args=parser.parse_args()
    if args.output.resolve() in (args.dll.resolve(),args.manifest.resolve()):
        parser.error('--output must differ from both input paths')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    result={'status':'validation-incomplete','candidate_path':str(args.dll.resolve()),
            'manifest_path':str(args.manifest.resolve()),'tests':[]}
    # Invalidate any older result before parsing inputs or starting emulation.
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    try:
        if not __debug__:
            raise ValueError('Validation requires assertions; run Python without -O or -OO')
        result=validate(args.dll,args.manifest)
    except Exception as error:
        result.update({'status':'validation-incomplete','error':str(error)})
        args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({'status':result['status'],'error':str(error)},ensure_ascii=False))
        raise SystemExit(1) from error
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    rows=result['tests']
    print(json.dumps({'status':result['status'],'counts':result['counts'],
        'output':str(args.output.resolve()),
        'failures':[{'name':r['name'],'base':r['image_base'],'error':r.get('error')}for r in rows if r['status']!='passed']},ensure_ascii=False))
    if result['status']!='passed':raise SystemExit(1)


def validate(dll_path,manifest_path):
    """Run fresh simulations from the supplied files; never reuse prior results."""
    dll=dll_path.read_bytes()
    manifest_raw=manifest_path.read_bytes()
    spec=specification(json.loads(manifest_raw))
    candidate_hash=hashlib.sha256(dll).hexdigest()
    if candidate_hash.lower()!=spec['candidate_sha256'].lower():
        raise ValueError('Candidate SHA-256 does not match the supplied manifest')
    header=pefile.PE(data=dll,fast_load=True)
    if header.FILE_HEADER.Machine!=0x8664 or header.OPTIONAL_HEADER.Magic!=0x20B:
        raise ValueError('Expected an AMD64 PE32+ candidate')
    preferred_base=header.OPTIONAL_HEADER.ImageBase
    relocated_base=0x7F0000000000
    if preferred_base==relocated_base:
        raise ValueError('Preferred base collides with the distinct relocation test base')
    rows=[]
    for image_base in (preferred_base,relocated_base):
        rows.extend(scenarios(dll,spec,image_base))
    return {'status':'passed' if all(row['status']=='passed'for row in rows) else 'failed',
        'completed_utc':datetime.now(timezone.utc).isoformat(),
        'method':'Unicorn x86-64 execution of candidate wrapper instructions with hostile ABI-conforming stubs',
        'candidate_path':str(dll_path.resolve()),'candidate_sha256':candidate_hash,
        'manifest_path':str(manifest_path.resolve()),'manifest_sha256':hashlib.sha256(manifest_raw).hexdigest(),
        'resolved_specification':spec,'unicorn_version':unicorn.__version__,
        'counts':dict(Counter(row['status']for row in rows)),'tests':rows,
        'limits':['Emulator fixtures verify wrapper control flow and ABI, not real-game timing or rendering.',
                  'Original FrameStage, SRW functions and cache insertion are controlled stubs; their full implementations are not executed.',
                  'No target process is opened and the DLL is not loaded through the operating system.']}


if __name__=='__main__':main()
