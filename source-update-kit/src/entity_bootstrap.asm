; Executed repair source, assembled by src/build.py using Keystone.
; @rva:name@ is a module-relative call target; @mem:name@ is a RIP-relative memory operand.
; @value:name@ is a profile parameter. All symbols are in the selected JSON profile.
; Preserve the reviewed prologue/epilogue unless unwind metadata and validation are revised.
; First ready frame only; no empty-cache retry loop.

        push rbx
        push rsi
        push rdi
        push r12
        sub rsp, 0x28
        mov ebx, edx
        call @rva:original_frame@
        mov qword ptr [rsp + 0x20], rax
        cmp ebx, @value:ready_stage@
        jne finish
        cmp dword ptr @mem:state_status@, 0
        jne finish
        test byte ptr @mem:loading_gate@, @value:ready_mask@
        jnz finish
        test byte ptr @mem:frame_ready@, @value:ready_mask@
        jz finish
        mov rax, qword ptr @mem:entity_system_slot@
        test rax, rax
        jz finish
        cmp rax, -1
        je finish
        mov rax, qword ptr [rax]
        test rax, rax
        jz finish
        cmp rax, -1
        je finish
        lea rcx, @mem:context_lock@
        call qword ptr @mem:acquire_shared_iat@
        mov r12, qword ptr @mem:controller@
        mov rsi, qword ptr @mem:pawn@
        or rsi, qword ptr @mem:observer_pawn@
        lea rcx, @mem:context_lock@
        call qword ptr @mem:release_shared_iat@
        test r12, r12
        jz finish
        test rsi, rsi
        jz finish
        xor eax, eax
        mov edx, 1
        lock cmpxchg dword ptr @mem:state_status@, edx
        jne finish
        mov qword ptr @mem:state_controller@, r12
        lea rdi, @mem:entity_cache@
        xor esi, esi
    enumerate:
        mov rcx, rdi
        mov edx, esi
        call @rva:entity_lookup@
        test rax, rax
        jz next_entity
        mov rdx, rax
        mov r8d, esi
        mov rcx, rdi
        inc dword ptr @mem:state_nonzero_entity_count@
        call @rva:entity_add@
    next_entity:
        inc esi
        cmp esi, @value:entity_index_limit@
        jb enumerate
        mov dword ptr @mem:state_enumerated_index_limit@, esi
        mov rcx, rdi
        mov rdx, r12
        call @rva:entity_membership@
        test al, al
        jz missing_controller
        mov dword ptr @mem:state_status@, 2
        jmp finish
    missing_controller:
        mov dword ptr @mem:state_status@, 3
    finish:
        mov rax, qword ptr [rsp + 0x20]
        add rsp, 0x28
        pop r12
        pop rdi
        pop rsi
        pop rbx
        ret
