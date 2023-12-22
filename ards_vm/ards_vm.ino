#include "ards_vm.hpp"

#ifndef EEPROM_h
#define EEPROM_h
#endif
#include <Arduboy2.h>

#include "ards_tone.hpp"

Arduboy2Base a;

ARDUBOY_NO_USB

void setup()
{
    a.boot();
    FX::begin(0);
    
    ards::Tones::setup();
    
    // adjust if we are in dev mode
    if(FX::programDataPage == 0)
    {
        FX::readDataBytes(
            uint24_t(16) * 1024 * 1024 - 2,
            (uint8_t*)&FX::programDataPage,
            2);
        //FX::programDataPage = 65535;
    }
}

void loop()
{
    ards::vm_run();
}
