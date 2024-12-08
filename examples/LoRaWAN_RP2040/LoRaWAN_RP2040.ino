/*

This demonstrates how to save the join information in to permanent memory
so that if the power fails, batteries run out or are changed, the rejoin
is more efficient & happens sooner due to the way that LoRaWAN secures
the join process - see the wiki for more details.

This is typically useful for devices that need more power than a battery
driven sensor - something like a air quality monitor or GPS based device that
is likely to use up it's power source resulting in loss of the session.

The relevant code is flagged with a ##### comment

Saving the entire session is possible but not demonstrated here - it has
implications for flash wearing and complications with which parts of the 
session may have changed after an uplink. So it is assumed that the device
is going in to deep-sleep, as below, between normal uplinks.

Configure Flash Size to provide a file system (FS)! (64KB is sufficient)
*/

#if !defined(ARDUINO_ARCH_RP2040)
  #pragma error ("This is not the example your device is looking for - RP2040 only")
#endif

#include "src/rp2040/pico_rtc_utils.h"

// ##### load the preferences facilites (https://github.com/vshymanskyy/Preferences)
#include <Preferences.h>
Preferences store;

// LoRaWAN config, credentials & pinmap
#include "config.h" 

#include <RadioLib.h>

// Utilities & vars to support deep-sleep. Putting values into the .uninitialized_data
// section prevents that those values are changed after soft reset.
// This means normal initialization won't work.
// As a workaround, we use the watchdog scratch register 1 to store bootCount, which is reset to 0
// by HW and maintains its value after a SW reset.
uint16_t bootCount;
uint16_t bootCountSinceUnsuccessfulJoin __attribute__((section(".uninitialized_data"))); // init 0
uint8_t LWsession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] __attribute__((section(".uninitialized_data")));


// Sleep for <seconds>
// Note:
// RP2040 RTC wake-up is triggered when the programmed time and date is reached.
// Please not that the RP2040 RTC is reset even by a soft reset! (sic!)
void gotoSleep(uint32_t seconds)
{
  watchdog_hw->scratch[1] = bootCount;
  Serial.print(F("Sleeping for "));
  Serial.print(seconds);
  Serial.println(F(" seconds"));
  time_t t_now = 0;
  datetime_t dt;
  epoch_to_datetime(&t_now, &dt);
  rtc_set_datetime(&dt);
  delay(100);
  pico_sleep(seconds);
}

// setup ...
void setup() {
  Serial.begin(115200);
  while (!Serial);  							// wait for serial to be initalised
  delay(2000);  									// give time to switch to the serial monitor
  Serial.println(F("\nSetup"));
  
  // see pico-sdk/src/rp2_common/hardware_rtc/rtc.c
  rtc_init();
  sleep_us(64);

  bootCount = watchdog_hw->scratch[1];
  if (bootCount == 0) {
    // HW reset occurred, initialize variables in .uninitialized_data section
    bootCount = 1;
    bootCountSinceUnsuccessfulJoin = 0;
    
    // Note: LWsession does not require initialization
  }

  int16_t state = 0;  						// return value for calls to RadioLib

  // setup the radio based on the pinmap (connections) in config.h
  Serial.println(F("Initalise the radio"));
  state = radio.begin();
  debug(state != RADIOLIB_ERR_NONE, F("Initalise radio failed"), state, true);

  Serial.println(F("Recalling LoRaWAN nonces & session"));
  // ##### setup the flash storage
  store.begin("radiolib");
  // ##### if we have previously saved nonces, restore them
  if (store.isKey("nonces")) {
    uint8_t buffer[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];										// create somewhere to store nonces
    store.getBytes("nonces", buffer, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);	// get them to the store
    state = node.setBufferNonces(buffer); 															// send them to LoRaWAN
    debug(state != RADIOLIB_ERR_NONE, F("Restoring nonces buffer failed"), state, false);
  }

  // recall session from RAM deep-sleep preserved variable
  state = node.setBufferSession(LWsession); // send them to LoRaWAN stack
  // if we have booted at least once we should have a session to restore, so report any failure
  // otherwise no point saying there's been a failure when it was bound to fail with an empty 
  // LWsession var. At this point, bootCount has already been incremented, hence the > 2
  debug((state != RADIOLIB_ERR_NONE) && (bootCount > 2), F("Restoring session buffer failed"), state, false);

  // process the restored session or failing that, create a new one & 
  // return flag to indicate a fresh join is required
  Serial.println(F("Setup LoRaWAN session"));
  state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey, false);
  // see comment above, no need to report a failure that is bound to occur on first boot
  debug((state != RADIOLIB_ERR_NONE) && (bootCount > 2), F("Restore session failed"), state, false);

  // loop until successful join
  while (state != RADIOLIB_ERR_NONE) {
    Serial.println(F("Join ('login') to the LoRaWAN Network"));
    state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey, true);

    if (state < RADIOLIB_ERR_NONE) {
      Serial.print(F("Join failed: "));
      Serial.println(state);

      // how long to wait before join attempts. This is an interim solution pending 
      // implementation of TS001 LoRaWAN Specification section #7 - this doc applies to v1.0.4 & v1.1
      // it sleeps for longer & longer durations to give time for any gateway issues to resolve
      // or whatever is interfering with the device <-> gateway airwaves.
      uint32_t sleepForSeconds = min((bootCountSinceUnsuccessfulJoin++ + 1UL) * 60UL, 3UL * 60UL);
      Serial.print(F("Boots since unsuccessful join: "));
      Serial.println(bootCountSinceUnsuccessfulJoin);
      Serial.print(F("Retrying join in "));
      Serial.print(sleepForSeconds);
      Serial.println(F(" seconds"));

      gotoSleep(sleepForSeconds);

    } else {  // join was successful
        Serial.println(F("Joined"));

        // ##### save the join counters (nonces) to permanent store
        Serial.println(F("Saving nonces to flash"));
        uint8_t buffer[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];           // create somewhere to store nonces
        uint8_t *persist = node.getBufferNonces();                  // get pointer to nonces
        memcpy(buffer, persist, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);  // copy in to buffer
        store.putBytes("nonces", buffer, RADIOLIB_LORAWAN_NONCES_BUF_SIZE); // send them to the store

        // we'll save the session after the uplink

        // reset the failed join count
        bootCountSinceUnsuccessfulJoin = 0;

        delay(1000);  // hold off off hitting the airwaves again too soon - an issue in the US

    } // if beginOTAA state
  } // while join

  // ##### close the store
  store.end();  
  

  // ----- and now for the main event -----
  Serial.println(F("Sending uplink"));

  // build payload byte array
  uint8_t uplinkPayload[4];
  uplinkPayload[0] = 0xDE;
  uplinkPayload[1] = 0xAD;
  uplinkPayload[2] = 0xBE;
  uplinkPayload[3] = 0xEF;
  
  // perform an uplink
  state = node.sendReceive(uplinkPayload, sizeof(uplinkPayload));    
  debug((state != RADIOLIB_LORAWAN_NO_DOWNLINK) && (state != RADIOLIB_ERR_NONE), F("Error in sendReceive"), state, false);

  Serial.print(F("FcntUp: "));
  Serial.println(node.getFcntUp());

  // now save session to RTC memory
  uint8_t *persist = node.getBufferSession();
  memcpy(LWsession, persist, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  
  // wait until next uplink - observing legal & TTN FUP constraints
  gotoSleep(uplinkIntervalSeconds);

  // --------------------------------------------------------------------------
  // The RP2040 is actually capable of resuming normal operation after wake-up,
  // so the main part of the sketch could be executed in loop().
  // Unfortunately, the correct way to restore the clocks after wake-up still
  // has to be found.
  // --------------------------------------------------------------------------

  // Soft reset
  rp2040.restart();
}


// The RP2040 wakes from deep-sleep and executes a soft reset - this mimics
// the ESP32's behavior.
// It then goes back to sleep, so loop() is never called and which is
// why it is empty.
void loop() {

}
