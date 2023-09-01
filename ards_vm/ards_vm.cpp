#include "ards_vm.hpp"
#include "ards_instr.hpp"

#include <Arduboy2.h>

#define SPRITESU_IMPLEMENTATION
#define SPRITESU_FX
#define SPRITESU_RECT
#include "SpritesU.hpp"

namespace ards
{

vm_t vm;

static inline uint8_t read_u8()
{
    vm.pc += 1;
    return FX::readPendingUInt8();
}

static inline uint16_t read_u16()
{
    vm.pc += 2;
    uint16_t t = FX::readPendingUInt8();
    t += (uint16_t(FX::readPendingUInt8()) << 8);
    return t;
}

static inline uint24_t read_u24()
{
    vm.pc += 3;
    uint24_t t = FX::readPendingUInt8();
    t += (uint24_t(FX::readPendingUInt8()) << 8);
    t += (uint24_t(FX::readPendingUInt8()) << 16);
    return t;
}

static inline uint16_t conv2(uint8_t a, uint8_t b)
{
    return uint16_t(a) + (uint16_t(b) << 8);
}

static inline uint24_t conv3(uint8_t a, uint8_t b, uint8_t c)
{
    return uint24_t(a) + (uint24_t(b) << 8) + (uint24_t(c) << 16);
}

inline uint8_t* vm_pop_begin()
{
    uint8_t* r;
    asm volatile(
        "lds  %A[r], 0x0660\n"
        "ldi  %B[r], 1\n"
        : [r] "=&d" (r));
    return r;
    //return &vm.stack[vm.sp];
}

inline void vm_pop_end(uint8_t* ptr)
{
    vm.sp = (uint8_t)(uintptr_t)ptr;
}

template<class T>
__attribute__((always_inline)) inline T vm_pop(uint8_t*& ptr)
{
    union
    {
        T r;
        uint8_t b[sizeof(T)];
    } u;
    for(size_t i = 0; i < sizeof(T); ++i)
    {
        asm volatile(
            "ld %[t], -%a[ptr]\n"
            : [t] "=&r" (u.b[sizeof(T) - i - 1])
            , [ptr] "+&e" (ptr)
            );
    }
    return u.r;
}

template<class T>
inline void vm_push(T x)
{
    for(size_t i = 0; i < sizeof(T); ++i)
    {
        vm.stack[vm.sp++] = uint8_t(x);
        x >>= 8;
    }
}

using sys_func_t = void(*)();

static sys_display()
{
    (void)FX::readEnd();
    FX::display(true);
    FX::seekData(vm.pc);
}

static sys_draw_pixel()
{
    auto ptr = vm_pop_begin();
    uint8_t color = vm_pop<uint8_t>(ptr);
    int16_t y = vm_pop<int16_t>(ptr);
    int16_t x = vm_pop<int16_t>(ptr);
    vm_pop_end(ptr);
    Arduboy2Base::drawPixel(x, y, color);
}

static sys_draw_filled_rect()
{
    auto ptr = vm_pop_begin();
    uint8_t color = vm_pop<uint8_t>(ptr);
    uint8_t h = vm_pop<uint8_t>(ptr);
    uint8_t w = vm_pop<uint8_t>(ptr);
    int16_t y = vm_pop<int16_t>(ptr);
    int16_t x = vm_pop<int16_t>(ptr);
    vm_pop_end(ptr);
    SpritesU::fillRect(x, y, w, h, color);
}

static sys_set_frame_rate()
{
    auto ptr = vm_pop_begin();
    uint8_t fr = vm_pop<uint8_t>(ptr);
    vm_pop_end(ptr);
    Arduboy2Base::setFrameRate(fr);
}

static sys_next_frame()
{
    bool x = Arduboy2Base::nextFrame();
    vm_push(uint8_t(x));
}

static sys_idle()
{
    Arduboy2Base::idle();
}

static sys_debug_break()
{
    asm volatile("break\n");
}

static sys_func_t const SYS_FUNCS[] __attribute__((aligned(256))) PROGMEM =
{
    sys_display,
    sys_draw_pixel,
    sys_draw_filled_rect,
    sys_set_frame_rate,
    sys_next_frame,
    sys_idle,
    sys_debug_break,
};

// main assembly method containing optimized implementations for each instruction
// the alignment of 512 allows optimized dispatch: the prog address for ijmp can
// be computed by adding to the high byte only instead of adding a 2-byte offset

static void __attribute__((used, naked, aligned(512))) vm_execute()
{
    asm volatile(
    
R"(

vm_execute:

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; Registers
; 
;     r0-r1        - scratch regs
;     r2           - constant value 0
;     r3           - constant value 32 (used for dispatch)
;     r4           - constant value 1
;     r5           - constant value hi8(vm_execute) TODO :/
;     r6-r8        - pc
;     r9-r24       - scratch regs
;     r25          - reserved for TOS (TODO)
;     r26-r27      - scratch regs
;     r28:29       - &vm.stack[sp] (sp is r28)
;     r30-r31      - scratch regs
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; helper macros
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .macro dispatch_delay
    lpm
    lpm
    nop
    .endm
    
    .macro fx_disable
    sbi  %[fxport], %[fxbit]
    .endm
    
    .macro fx_enable
    cbi  %[fxport], %[fxbit]
    .endm
    
    .macro read_end
    in   r0, %[spsr]
    sbrs r0, 7
    rjmp .-6
    sbi  %[fxport], %[fxbit]
    .endm
    
    .macro read_byte
    in   r0, %[spdr]
    out  %[spdr], r2
    add  r6, r4
    adc  r7, r2
    adc  r8, r2
    .endm
    
    .macro dispatch_noalign
    read_byte
    mul  r0, r3
    movw r30, r0
    add  r31, r5
    ijmp
    .endm
    
    ; need to copy macro here because max nesting is 2 apparently
    .macro dispatch
    read_byte
    mul  r0, r3
    movw r30, r0
    add  r31, r5
    ijmp
    .align 6
    .endm

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; instructions
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

I_NOP:
    dispatch_delay
    dispatch

I_PUSH:
    dispatch_delay
    read_byte
    st   Y+, r0
    call delay_11
    dispatch

I_P0:
    st   Y+, r2
    lpm
    rjmp .+0
    dispatch

I_P1:
    ldi  r16, 1
    st   Y+, r16
    rjmp .+0
    rjmp .+0
    dispatch

I_P2:
    ldi  r16, 2
    st   Y+, r16
    rjmp .+0
    rjmp .+0
    dispatch

I_P3:
    ldi  r16, 3
    st   Y+, r16
    rjmp .+0
    rjmp .+0
    dispatch

I_P4:
    ldi  r16, 4
    st   Y+, r16
    rjmp .+0
    rjmp .+0
    dispatch

I_P00:
    st   Y+, r2
    st   Y+, r2
    lpm
    dispatch

I_SEXT:
    ld   r0, -Y
    inc  r28
    ldi  r16, 0xff
    sbrs r0, 7
    ldi  r16, 0x00
    st   Y+, r16
    dispatch
 
I_DUP:
    ld   r0, -Y
    st   Y+, r0
    st   Y+, r0
    nop
    dispatch
   
I_GETL:
    lpm
    lpm
    movw r26, r28
    read_byte
    sub  r26, r0
    ld   r0, X
    st   Y+, r0
    lpm
    lpm
    nop
    dispatch
    
I_GETL2:
    lpm
    lpm
    movw r26, r28
    read_byte
    sub  r26, r0
    ld   r0, X+
    ld   r1, X
    st   Y+, r0
    st   Y+, r1
    lpm
    dispatch

I_GETLN:
    dispatch_delay
    read_byte
    ld   r16, -Y
    movw r26, r28
    sub  r26, r0
1:  ld   r0, X+
    st   Y+, r0
    dec  r16
    brne 1b
    call delay_8 ; TODO: remove this when GETLN(1) is not allowed
    dispatch

I_SETL:
    dispatch_delay
    read_byte
    ld   r16, -Y
    movw r26, r28
    sub  r26, r0
    st   X, r16
    lpm
    lpm
    nop
    dispatch
 
I_SETLN:
    dispatch_delay
    read_byte
    ld   r16, -Y
    movw r26, r28
    sub  r26, r0
1:  ld   r0, -Y
    st   -X, r0
    dec  r16
    brne 1b
    call delay_8 ; TODO: remove this when SETLN(1) is not allowed
    dispatch

I_GETG:
    call read_2_bytes
    movw r26, r16
    subi r27, -2
    ld   r16, X
    st   Y+, r16
    call delay_11
    dispatch

I_GETGN:
    call read_2_bytes
    movw r26, r16
    subi r27, -2
    ld   r16, -Y
1:  ld   r0, X+
    st   Y+, r0
    dec  r16
    brne 1b
    call delay_8 ; TODO: remove this when GETGN(1) is not allowed
    dispatch

I_SETG:
    call read_2_bytes
    movw r26, r16
    subi r27, -2
    ld   r16, -Y
    st   X, 16
    call delay_11
    dispatch

I_SETGN:
    call read_2_bytes
    movw r26, r16
    subi r27, -2
    ld   r16, -Y
    add  r26, r16
    adc  r27, r2
1:  ld   r0, -Y
    st   -X, r0
    dec  r16
    brne 1b
    call delay_8 ; TODO: remove this when SETGN(1) is not allowed
    dispatch

I_GETR:
    ld   r27, -Y
    ld   r26, -Y
    ld   r1, X+
    st   Y+, r1
    dispatch

I_GETR2:
    ld   r27, -Y
    ld   r26, -Y
    ld   r1, X+
    st   Y+, r1
    ld   r1, X+
    st   Y+, r1
    dispatch

I_GETRN:
    dispatch_delay
    read_byte
    ld   r27, -Y
    ld   r26, -Y
1:  ld   r1, X+
    st   Y+, r1
    dec  r0
    brne 1b
    lpm ; TODO: remove this when GETRN(1) is not allowed
    dispatch

I_SETR:
    ld   r27, -Y
    ld   r26, -Y
    ld   r1, -Y
    st   X, r1
    dispatch

I_SETR2:
    ld   r27, -Y
    ld   r26, -Y
    ld   r1, -Y
    ld   r0, -Y
    st   X+, r0
    st   X, r1
    dispatch

I_SETRN:
    dispatch_delay
    read_byte
    ld   r27, -Y
    ld   r26, -Y
    add  r26, r0
    adc  r26, r2
1:  ld   r1, -Y
    st   -X, r1
    dec  r0
    brne 1b
    nop ; TODO: remove this when SETRN(1) is not allowed
    dispatch

I_POP:
    dec  r28
    lpm
    lpm
    dispatch

I_POP2:
    subi r28, 2
    lpm
    lpm
    dispatch

I_POP3:
    subi r28, 3
    lpm
    lpm
    dispatch

I_POP4:
    subi r28, 4
    lpm
    lpm
    dispatch

I_AIDXB:
    ld   r20, -Y
    nop
    call read_2_bytes_nodelay
    ; r16: elem size
    ; r17: num elems
    ; r20: index
    cp   r20, r17
    brlo 1f
    rjmp .-2 ; TODO: actual error jump
1:  mul  r16, r20
    movw r22, r0
    ld   r21, -Y
    ld   r20, -Y
    add  r22, r20
    adc  r23, r21
    st   Y+, r22
    st   Y+, r23
    dispatch

I_AIDX:
    ld   r21, -Y
    ld   r20, -Y
    call read_4_bytes_nodelay
    cp   r20, r18
    cpc  r21, r19
    brlo 1f
    rjmp .-2 ; TODO: actual error jump
    ; A1 A0 : r21 r20
    ; B1 B0 : r17 r16
    ; C1 C0 : r23 r22
    ;
    ;    A1 A0
    ;    B1 B0
    ;    =====
    ;    A0*B0
    ; A1*B0
    ; A0*B1
    ; ========
    ;    C1 C0
    ;
1:  mul  r16, r20
    movw r22, r0
    mul  r16, r21
    add  r23, r0
    mul  r17, r20
    add  r23, r0
    ld   r21, -Y
    ld   r20, -Y
    add  r22, r20
    adc  r23, r21
    st   Y+, r22
    st   Y+, r23
    dispatch

I_REFL:
    dispatch_delay
    read_byte
    movw r16, r28
    sub  r16, r0
    st   Y+, r16
    st   Y+, r17
    lpm
    lpm
    nop
    dispatch

I_REFG:
    call read_2_bytes
    subi r17, -2
    st   Y+, r16
    st   Y+, r17
    lpm
    lpm
    nop
    dispatch

I_REFGB:
    dispatch_delay
    read_byte
    ldi  r17, 2
    st   Y+, r0
    st   Y+, r17
    lpm
    lpm
    rjmp .+0
    dispatch

I_INC:
    ld   r0, -Y
    inc  r0
    st   Y+, r0
    rjmp .+0
    dispatch

I_ADD:
    ld   r10, -Y
    ld   r14, -Y
    add  r14, r10
    st   Y+, r14
    dispatch

I_ADD2:
    ld   r11, -Y
    ld   r10, -Y
    ld   r15, -Y
    ld   r14, -Y
    add  r14, r10
    adc  r15, r11
    st   Y+, r14
    st   Y+, r15
    dispatch

I_ADD3:
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    add  r14, r10
    adc  r15, r11
    adc  r16, r12
    st   Y+, r14
    st   Y+, r15
    st   Y+, r16
    dispatch

I_ADD4:
    ld   r13, -Y
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r17, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    add  r14, r10
    adc  r15, r11
    adc  r16, r12
    adc  r17, r13
    st   Y+, r14
    st   Y+, r15
    st   Y+, r16
    st   Y+, r17
    dispatch

I_SUB:
    ld   r10, -Y
    ld   r14, -Y
    sub  r14, r10
    st   Y+, r14
    dispatch

I_SUB2:
    ld   r11, -Y
    ld   r10, -Y
    ld   r15, -Y
    ld   r14, -Y
    sub  r14, r10
    sbc  r15, r11
    st   Y+, r14
    st   Y+, r15
    dispatch

I_SUB3:
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    sub  r14, r10
    sbc  r15, r11
    sbc  r16, r12
    st   Y+, r14
    st   Y+, r15
    st   Y+, r16
    dispatch

I_SUB4:
    ld   r13, -Y
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r17, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    sub  r14, r10
    sbc  r15, r11
    sbc  r16, r12
    sbc  r17, r13
    st   Y+, r14
    st   Y+, r15
    st   Y+, r16
    st   Y+, r17
    dispatch

I_MUL:
    ld   r10, -Y
    ld   r14, -Y
    mul  r14, r10
    st   Y+, r0
    dispatch

I_MUL2:
    ;
    ;    A1 A0
    ;    B1 B0
    ;    =====
    ;    A0*B0
    ; A1*B0
    ; A0*B1
    ; ========
    ;    C1 C0
    ;
    ld   r11, -Y
    ld   r10, -Y
    ld   r15, -Y
    ld   r14, -Y
    mul  r14, r10 ; A0*B0
    movw r18, r0
    mul  r14, r11 ; A0*B1
    add  r19, r0
    mul  r15, r10 ; A1*B0
    add  r19, r0
    st   Y+, r18
    st   Y+, r19
    dispatch

I_MUL3:
    ;   
    ;    A2 A1 A0
    ;    B2 B1 B0
    ;    ========
    ;       A0*B0
    ;    A1*B0
    ;    A0*B1
    ; A2*B0
    ; A1*B1
    ; A0*B2
    ; ===========
    ;    C2 C1 C0
    ;   
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    mul  r14, r10 ; A0*B0
    movw r18, r0
    mul  r16, r10 ; A2*B0
    mov  r20, r0
    mul  r15, r10 ; A1*B0
    add  r19, r0
    adc  r20, r1
    mul  r14, r11 ; A0*B1
    add  r19, r0
    adc  r20, r1
    mul  r15, r11 ; A1*B1
    add  r20, r0
    mul  r14, r12 ; A0*B2
    add  r20, r0
    st   Y+, r18
    st   Y+, r19
    st   Y+, r20
    dispatch

I_MUL4:
    jmp  instr_mul4
    .align 6

I_BOOL:
    ld   r0, -Y
    ldi  r16, 0
    cpse r0, r2
    ldi  r16, 1
    st   Y+, r16
    dispatch

I_BOOL2:
    ld   r0, -Y
    ld   r1, -Y
    or   r0, r1
    ldi  r16, 0
    cpse r0, r2
    ldi  r16, 1
    st   Y+, r16
    dispatch

I_BOOL3:
    ld   r0, -Y
    ld   r1, -Y
    or   r0, r1
    ld   r1, -Y
    or   r0, r1
    ldi  r16, 0
    cpse r0, r2
    ldi  r16, 1
    st   Y+, r16
    dispatch

I_BOOL4:
    ld   r0, -Y
    ld   r1, -Y
    or   r0, r1
    ld   r1, -Y
    or   r0, r1
    ld   r1, -Y
    or   r0, r1
    ldi  r16, 0
    cpse r0, r2
    ldi  r16, 1
    st   Y+, r16
    dispatch

I_CULT:
    ld   r10, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    brlo 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULT2:
    ld   r11, -Y
    ld   r10, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    brlo 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULT3:
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    cpc  r16, r12
    brlo 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULT4:
    ld   r13, -Y
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r17, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    cpc  r16, r12
    cpc  r17, r13
    brlo 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLT:
    ld   r10, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    brlt 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLT2:
    ld   r11, -Y
    ld   r10, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    brlt 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLT3:
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    cpc  r16, r12
    brlt 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLT4:
    ld   r13, -Y
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r17, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r14, r10
    cpc  r15, r11
    cpc  r16, r12
    cpc  r17, r13
    brlt 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULE:
    ld   r10, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r10, r14
    brsh 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULE2:
    ld   r10, -Y
    ld   r11, -Y
    ld   r14, -Y
    ld   r15, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    brsh 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULE3:
    ld   r10, -Y
    ld   r11, -Y
    ld   r12, -Y
    ld   r14, -Y
    ld   r15, -Y
    ld   r16, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    cpc  r12, r16
    brsh 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CULE4:
    ld   r10, -Y
    ld   r11, -Y
    ld   r12, -Y
    ld   r13, -Y
    ld   r14, -Y
    ld   r15, -Y
    ld   r16, -Y
    ld   r17, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    cpc  r12, r16
    cpc  r13, r17
    brsh 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLE:
    ld   r10, -Y
    ld   r14, -Y
    ldi  r18, 1
    cp   r10, r14
    brge 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLE2:
    ld   r10, -Y
    ld   r11, -Y
    ld   r14, -Y
    ld   r15, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    brge 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLE3:
    ld   r10, -Y
    ld   r11, -Y
    ld   r12, -Y
    ld   r14, -Y
    ld   r15, -Y
    ld   r16, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    cpc  r12, r16
    brge 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_CSLE4:
    ld   r10, -Y
    ld   r11, -Y
    ld   r12, -Y
    ld   r13, -Y
    ld   r14, -Y
    ld   r15, -Y
    ld   r16, -Y
    ld   r17, -Y
    ldi  r18, 1
    cp   r10, r14
    cpc  r11, r15
    cpc  r12, r16
    cpc  r13, r17
    brge 1f
    ldi  r18, 0
1:  st   Y+, r18
    dispatch

I_NOT:
    ld   r0, -Y
    ldi  r16, 1
    cpse r0, r2
    ldi  r16, 0
    st   Y+, r16
    dispatch

I_BZ:
    ld   r0, -Y
    nop
    call read_3_bytes_end_nodelay
    cp   r0, r2
    brne 1f
    movw r6, r16
    mov  r8, r18
    jmp  jump_to_pc
1:  out  %[spdr], r2
    call delay_17
    dispatch

I_BNZ:
    ld   r0, -Y
    nop
    call read_3_bytes_end_nodelay
    cp   r0, r2
    breq 1f
    movw r6, r16
    mov  r8, r18
    jmp  jump_to_pc
1:  out  %[spdr], r2
    call delay_17
    dispatch

I_JMP:
    call read_3_bytes_end
    movw r6, r16
    mov  r8, r18
    jmp  jump_to_pc
    .align 6

I_CALL:
    lds  r26, 0x0664
    ldi  r27, 0x06
    call read_3_bytes_end_nodelay
    st   X+, r6
    st   X+, r7
    st   X+, r8
    sts  0x0664, r26
    movw r6, r16
    mov  r8, r18
    jmp  jump_to_pc
    .align 6

I_RET:
    lds  r26, 0x0664
    ldi  r27, 0x06
    ld   r8, -X
    ld   r7, -X
    ld   r6, -X
    sts  0x0664, r26
    jmp  jump_to_pc
    .align 6

I_SYS:
    dispatch_delay
    read_byte
    mov  r30, r0
    ldi  r31, hi8(%[sys_funcs])
    call delay_11
    read_byte
    add  r31, r0
    lpm  r0, Z+
    lpm  r31, Z
    mov  r30, r0
    call store_vm_state
    icall
    call restore_vm_state
    jmp  dispatch_func
    .align 6

I_SYSB:
    ldi  r31, hi8(%[sys_funcs])
    lpm
    lpm
    read_byte
    mov  r30, r0
    lpm  r0, Z+
    lpm  r31, Z
    mov  r30, r0
    call store_vm_state
    icall
    call restore_vm_state
    jmp  dispatch_func
    .align 6

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; helper methods
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

instr_mul4:
    ;   
    ;    A3 A2 A1 A0
    ;    B3 B2 B1 B0
    ;    ===========
    ;          A0*B0
    ;       A1*B0
    ;       A0*B1
    ;    A2*B0
    ;    A1*B1
    ;    A0*B2
    ; A3*B0
    ; A2*B1
    ; A1*B2
    ; A0*B3
    ;    ===========
    ;    C3 C2 C1 C0
    ;   
    ld   r13, -Y
    ld   r12, -Y
    ld   r11, -Y
    ld   r10, -Y
    ld   r17, -Y
    ld   r16, -Y
    ld   r15, -Y
    ld   r14, -Y
    mul  r14, r10 ; A0*B0
    movw r18, r0
    mul  r16, r10 ; A2*B0
    movw r20, r0
    mul  r15, r10 ; A1*B0
    add  r19, r0
    adc  r20, r1
    adc  r21, r2
    mul  r14, r11 ; A0*B1
    add  r19, r0
    adc  r20, r1
    adc  r21, r2
    mul  r15, r11 ; A1*B1
    add  r20, r0
    adc  r21, r1
    mul  r14, r12 ; A0*B2
    add  r20, r0
    adc  r21, r1
    mul  r17, r10 ; A3*B0
    add  r21, r0
    mul  r16, r11 ; A2*B1
    add  r21, r0
    mul  r15, r12 ; A1*B2
    add  r21, r0
    mul  r14, r13 ; A0*B3
    add  r21, r0
    st   Y+, r18
    st   Y+, r19
    st   Y+, r20
    st   Y+, r21
    dispatch_noalign

read_4_bytes:
    lpm
read_4_bytes_nodelay:
    in   r16, %[spdr]
    out  %[spdr], r2
    ldi  r17, 4
    add  r6, r17
    adc  r7, r2
    adc  r8, r2
    call delay_12
    in   r17, %[spdr]
    out  %[spdr], r2
    call delay_16
    in   r18, %[spdr]
    out  %[spdr], r2
    call delay_16
    in   r19, %[spdr]
    out  %[spdr], r2
    ret

read_3_bytes:
    lpm
    in   r16, %[spdr]
    out  %[spdr], r2
    ldi  r17, 3
    add  r6, r17
    adc  r7, r2
    adc  r8, r2
    call delay_12
    in   r17, %[spdr]
    out  %[spdr], r2
    call delay_16
    in   r18, %[spdr]
    out  %[spdr], r2
    ret

read_3_bytes_end:
    lpm
read_3_bytes_end_nodelay:
    in   r16, %[spdr]
    out  %[spdr], r2
    ldi  r17, 3
    add  r6, r17
    adc  r7, r2
    adc  r8, r2
    call delay_12
    in   r17, %[spdr]
    out  %[spdr], r2
    call delay_16
    in   r18, %[spdr]
    ret

read_2_bytes:
    lpm
read_2_bytes_nodelay:
    in   r16, %[spdr]
    out  %[spdr], r2
    ldi  r17, 2
    add  r6, r17
    adc  r7, r2
    adc  r8, r2
    call delay_12
    in   r17, %[spdr]
    out  %[spdr], r2
    ret

    ; these two methods are used by the SYS instruction
store_vm_state:

    ; pc
    sts  0x0661, r6
    sts  0x0662, r7
    sts  0x0663, r8
    
    ; stack pointer: stack always begins at 0x100
    sts  0x0660, r28

    clr r1

    ret
    
restore_vm_state:

    ; 0 constant
    clr  r2
    
    ; 32 constant
    ldi  r16, 32
    mov  r3, r16
    
    ; 1 constant
    ldi  r16, 1
    mov  r4, r16
    
    ; pm_hi8(vm_execute) constant
    ldi  r16, pm_hi8(vm_execute)
    mov  r5, r16
    
    ; pc
    lds  r6, 0x0661
    lds  r7, 0x0662
    lds  r8, 0x0663
    
    ; stack pointer: stack always begins at 0x100
    lds  r28, 0x0660
    ldi  r29, 0x01

    ret

    ; the following delays assume a call via call, NOT rcall
    ; make sure you compile/link WITHOUT linker relaxing!
delay_17:
    nop
delay_16:
    nop
delay_15:
    nop
delay_14:
    nop
delay_13:
    nop
delay_12:
    nop
delay_11:
    nop
delay_10:
    nop
delay_9:
    nop
delay_8:
    ret
    
jump_to_pc:
    fx_disable
    lds  r16, %[data_page]+0
    lds  r17, %[data_page]+1
    add  r16, r7
    adc  r17, r8
    fx_enable
    ldi  r18, 3
    out  %[spdr], r18
    call delay_17
    out  %[spdr], r17
    call delay_17
    out  %[spdr], r16
    call delay_17
    out  %[spdr], r6
    call delay_17
    out  %[spdr], r2
    call delay_17
    dispatch
    
dispatch_func:
    dispatch_noalign

)"

    :
    : [spdr]       "i" (_SFR_IO_ADDR(SPDR))
    , [spsr]       "i" (_SFR_IO_ADDR(SPSR))
    , [fxport]     "i" (_SFR_IO_ADDR(FX_PORT))
    , [fxbit]      "i" (FX_BIT)
    , [data_page]  ""  (&FX::programDataPage)
    , [sys_funcs]  ""  (&SYS_FUNCS[0])
    );
}

void vm_run()
{
    Arduboy2Base::setFrameDuration(32);
    
    memset(&vm, 0, sizeof(vm));
    
    // read signature and refuse to run if it's not present
    {
        FX::seekData(0);
        uint32_t sig = FX::readPendingLastUInt32();
        if(sig != 0xABC00ABC)
            asm volatile("rjmp .-2\n"); // TODO: show a nice error screen
    }
    
    // skip signature
    *(volatile uint24_t*)&vm.pc = 4;

    // kick off execution
    asm volatile(R"(
    
        call restore_vm_state
        call jump_to_pc
        call dispatch_func
        jmp  0x0000
    )"
    :
    : [spdr]       "i" (_SFR_IO_ADDR(SPDR))
    , [spsr]       "i" (_SFR_IO_ADDR(SPSR))
    , [fxport]     "i" (_SFR_IO_ADDR(FX_PORT))
    , [fxbit]      "i" (FX_BIT)
    , [data_page]  ""  (&FX::programDataPage)
    );

}

}
