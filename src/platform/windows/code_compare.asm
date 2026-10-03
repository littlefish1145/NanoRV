; SPDX-License-Identifier: MIT
; Windows x64 leaf ABI: RCX=a, RDX=b, R8=bytes, EAX=equal.
; SSE2 is baseline on x64. Only volatile registers are modified; no stack,
; calls, nonvolatile SIMD registers, or unwind frame are needed. Never read
; beyond either snapshot, including a tail adjacent to an unmapped page.
bits 64
default rel
section .text align=16
global rvvm_code_bytes_equal
rvvm_code_bytes_equal:
    cmp r8,16
    jb .tail
.vector:
    movdqu xmm0,[rcx]
    movdqu xmm1,[rdx]
    pcmpeqb xmm0,xmm1
    pmovmskb eax,xmm0
    cmp eax,0xffff
    jne .different
    add rcx,16
    add rdx,16
    sub r8,16
    cmp r8,16
    jae .vector
.tail:
    test r8,r8
    jz .equal
.byte:
    mov al,[rcx]
    cmp al,[rdx]
    jne .different
    inc rcx
    inc rdx
    dec r8
    jnz .byte
.equal:
    mov eax,1
    ret
.different:
    xor eax,eax
    ret

; Called only after CPUID + OSXSAVE + XCR0 checks. Short snapshots use the
; SSE2 entry without touching YMM; all paths that do touch YMM issue VZEROUPPER
; before returning or entering legacy SSE code. Windows preserves only the
; low halves of XMM6-XMM15, none of which this leaf changes.
align 16
global rvvm_code_bytes_equal_avx2
rvvm_code_bytes_equal_avx2:
    cmp r8,32
    jb rvvm_code_bytes_equal
.vector:
    vmovdqu ymm0,[rcx]
    vpcmpeqb ymm0,ymm0,[rdx]
    vpmovmskb eax,ymm0
    cmp eax,-1
    jne .different
    add rcx,32
    add rdx,32
    sub r8,32
    cmp r8,32
    jae .vector
    vzeroupper
    jmp rvvm_code_bytes_equal
.different:
    vzeroupper
    xor eax,eax
    ret
