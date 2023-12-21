#pragma once

#ifndef EEPROM_h
#define EEPROM_h
#endif
#include <ArduboyFX.h>

namespace ards
{

enum error_t : uint8_t
{
    ERR_NONE,
    ERR_SIG, // signature error
    ERR_IDX, // array index out of bounds
    ERR_DIV, // division by zero
    ERR_ASS, // assertion failed
    ERR_DST, // data stack overflow
    ERR_CST, // call stack overflow
    ERR_FRM, // sprite frame outside of set
    ERR_STR, // source and dest string overlap
    NUM_ERR,
};

/*

building this requires a modified linker script:
    the .beforedata section must come before any other data section
    (that is, it must begin at 0x100, the origin of the data region)

making this change to the linker script (e.g., avr5.xn) will not affect
other projects that do not also place data into a ".beforedata" section

on my machine, the linker script is located at:
    C:\Program Files (x86)\Arduino\hardware\tools\avr\avr\lib\ldscripts\avr5.xn

and you can add the ".beforedata" section like this:

    ...
    
    .data : 
    {
        *(.beforedata)
        PROVIDE (__data_start = .) ;
        *(.data)
        *(.data*)
    
    ...
    
(adding it before __data_start makes the ELF debug info make more sense)
    
*/
extern __attribute__((section(".beforedata"))) struct vm_t
{
    uint8_t stack[256];    // 0x100
    uint8_t globals[1024]; // 0x200
    uint24_t calls[32];    // 0x600
    uint8_t sp;            // 0x660
    uint24_t pc;           // 0x661
    uint8_t csp;           // 0x664
    uint8_t error;         // 0x665
} vm;

void vm_run();

}
