#include "Arduino.h"
#include <sigma_delta.h>
#include <mmu_iram.h>
#include <ESP_EEPROM.h>

#include "apps/dazzledash.h" //- no picture
//#include "apps/midi.h" 
//#include "apps/gray.h"
//#include "apps/snake.h"
//#include "apps/font.h"
//#include "apps/circle.h"
//#include "apps/pong.h"
//#include "apps/tilemap.h"


#include "lib/ESPboyInit.h"
#include "lib/ESPboyInit.cpp"
#include "nbSPI.h"
#include "abc_interp.h"

#define APP_ID        0xDDDD
#define WIDTH         128
#define HEIGHT        64
#define SAMPLING_RATE 22000
#define SOUND_PIN     D3
#define SOUND_LEN     SAMPLING_RATE/10

static int16_t          samples[SOUND_LEN+1];
static volatile uint8_t endofSample = 1;
static volatile int16_t samplePointer = 0;

ESPboyInit myESPboy;

static abc_interp_t interp;
static abc_host_t host;


static inline __attribute__((always_inline)) uint8_t host_prog(void* user, uint32_t addr){
    return mmu_get_uint8((const void *)(&abcData[addr]));
}


static inline __attribute__((always_inline)) uint32_t host_millis(void* user){
    return millis();
}


static uint8_t __attribute__((always_inline)) host_buttons(void* user){
    static uint8_t nowkeys;
    nowkeys = myESPboy.getKeys();
    uint8_t b = 0;
    if(nowkeys&PAD_UP)    b |= ABC_BUTTON_U;
    if(nowkeys&PAD_DOWN)  b |= ABC_BUTTON_D;
    if(nowkeys&PAD_LEFT)  b |= ABC_BUTTON_L;
    if(nowkeys&PAD_RIGHT) b |= ABC_BUTTON_R;
    if(nowkeys&PAD_ACT)   b |= ABC_BUTTON_A;
    if(nowkeys&PAD_ESC)   b |= ABC_BUTTON_B;
    return b;
}


static uint32_t __attribute__((always_inline)) host_rand_seed(void* user){
  return analogRead(A0);
}


void __attribute__((always_inline)) doDisplayCPP(){   
    static uint16_t doblebuffer[128*128];
    ESP.wdtFeed();
    
    uint16_t addr1=0, addr2=WIDTH, addr3=0;
    for(uint8_t j=0; j<HEIGHT; j++){
      for(uint8_t i=0; i<WIDTH; i++){
        uint8_t bte = interp.display[addr3++];    
        doblebuffer[addr1++] = bte;
        doblebuffer[addr2++] = bte;
      }
      addr1+=WIDTH;
      addr2+=WIDTH;
    }
     
     while(nbSPI_isBusy()); 
     nbSPI_writeBytes((uint8_t*)doblebuffer, 128*128*2);
}


void IRAM_ATTR sound_ISR(){
     static uint8_t div = 1;
     static int16_t add = 0; 
     static int16_t sample;

     sample = samples[samplePointer];

     /* normalization */
     sample += add;
     if (sample < 0) add -= sample; 
     sample /= div;
     if (sample > 255) div++;
     
     sigmaDeltaWrite(0, sample);
     samplePointer++;

     /* command to prefill sound buffer */
     if(samplePointer == SOUND_LEN){
       samplePointer = 0;
       endofSample = 1;
     }    
     if(samplePointer == SOUND_LEN/2){
       endofSample = 2;
     }
}

   
void host_save(void* user, abc_interp_t const* interp){
      /* check if game saving and save if needed */
      //Serial.println("Saving done!");
      ESP.wdtFeed();
      noInterrupts();
      for(uint16_t i=0; i<1024; i++){
         EEPROM.write(i+2, interp->saved[i]);
      }
      EEPROM.commit();
      interrupts();
}


void fillSoundBuf(){
  if(endofSample == 2){
    abc_audio(&interp, &host, &samples[0], SOUND_LEN/2, SAMPLING_RATE);
    endofSample = 0;
  }
  if(endofSample == 1){
    abc_audio(&interp, &host, &samples[SOUND_LEN/2], SOUND_LEN/2, SAMPLING_RATE);
    endofSample = 0;
  }
}


void setup() {
  //Serial.begin(115200);
  EEPROM.begin(1028);

  /* Start ESPboy */
  myESPboy.begin("Dazzle Dash");
  //Serial.println("\nStart");

  /* Init sound */
  sigmaDeltaSetup(0, SAMPLING_RATE);
  sigmaDeltaAttachPin(SOUND_PIN);
  sigmaDeltaEnable();
  noInterrupts();
  timer1_isr_init();
  timer1_attachInterrupt(sound_ISR);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  timer1_write(80 * 1000000 / SAMPLING_RATE);//AUDIO_SAMPLE_RATE);
  interrupts();

  /* Init ABC */
  memset(&interp, 0, sizeof(interp));
  memset(&host, 0, sizeof(host));
  host.prog = host_prog;
  host.millis = host_millis;
  host.buttons = host_buttons;
  host.rand_seed = host_rand_seed;
  host.save = host_save;
  

  /* Init savings */
  uint16_t appId;
  EEPROM.get(0, appId);
  if (appId == APP_ID){
    //Serial.println("Loading done!");
    for(uint16_t i=0; i<1024; i++){
      interp.saved[i] = EEPROM.read(i+2);
      //Serial.print(interp.saved[i]); Serial.print("; ");
    }
    //Serial.println("\n\n");
    interp.has_save = 1;}
  else{
    //Serial.println("Init loading done!");
    appId = APP_ID;
    EEPROM.put(0, appId);
    interp.audio_disabled = 0;
    for(uint16_t i=0; i<1024; i++){
      interp.saved[i] = 0;
      EEPROM.write(i+2, interp.saved[i]);
    }
    EEPROM.commit();
  };

  /* Init display */
  myESPboy.tft.fillScreen(TFT_BLACK);
  myESPboy.tft.setAddrWindow(0, 0, WIDTH, HEIGHT*2);

}

 
void loop(){
  static uint8_t t;
  //for FPS display
  //static uint16_t frm = 0, oldtme = millis();

  /* do interp */
  for(uint16_t i = 0; i < 200; i++){     
    t = abc_run(&interp, &host);
    if (t == ABC_RESULT_IDLE)  {
      doDisplayCPP(); 
      // Serial.println("Idle");
      // for FPS display
      /*
      frm++;
      if(millis()>oldtme+1000){
      Serial.println(frm);  
      oldtme = millis();
      frm=0;
      }
      */
    }
    if (t == ABC_RESULT_BREAK) {/*Serial.println("Break");*/ delay(500);}
    if (t == ABC_RESULT_ERROR) {/*Serial.println("Error");*/ delay(5000); ESP.reset();}

    /* prefill sound buffer if get command form sound ISR */
    fillSoundBuf();
  }
}
