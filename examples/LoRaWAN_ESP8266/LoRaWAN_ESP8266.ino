
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

This example uses deep sleep mode, so connect D0=GPIO16 and RST
pins before running it.

*/

#if !defined(ESP8266)
  #pragma error ("This is not the example your device is looking for - ESP8266 only")
#endif

// ##### load the ESP8266 persistent storage facilites
#include <EEPROM.h>

// LoRaWAN config, credentials & pinmap
#include "config.h" 

#include <RadioLib.h>

// utilities & vars to support ESP8266 deep-sleep. 
// the contents of these variables is restored from RTC RAM upon deepsleep wake
uint8_t LWsession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
uint32_t bootCount = 1;
uint32_t bootCountSinceUnsuccessfulJoin = 0;

// print a human-readable format of the wakeup cause: 
// external (reset) or Deep-sleep (timer / button)
void print_wakeup_reason() {
  Serial.print(F("Wake caused by: "));
  Serial.println(ESP.getResetReason());
  if(ESP.getResetReason() != "Deep-Sleep Wake") {
    Serial.println("Initialising RTC RAM to 0");
    uint8_t rtcMemory[512] = { 0 };
    if(!ESP.rtcUserMemoryWrite(0, (uint32_t *)&rtcMemory, sizeof(rtcMemory))) {
      Serial.println("RTC write failed!");
    }
  }

}

// put device in to lowest power deep-sleep mode
void gotoSleep(uint32_t seconds) {
  uint32_t address = 0;
  Serial.println(F("Storing bootcount variables in RTC RAM"));
  if(!ESP.rtcUserMemoryWrite(address, &bootCount, sizeof(bootCount))) {
    Serial.println("RTC write failed!");
  }
  address += sizeof(bootCount);
  if(!ESP.rtcUserMemoryWrite(address, &bootCountSinceUnsuccessfulJoin, sizeof(bootCountSinceUnsuccessfulJoin))) {
    Serial.println("RTC write failed!");
  }

  Serial.println(F("Sleeping\n"));
  Serial.flush();

  ESP.deepSleep(seconds * 1000UL * 1000UL); // function uses uS

  // if this appears in the serial debug, we didn't go to sleep!
  // so take defensive action so we don't continually uplink
  Serial.println(F("\n\n### Sleep failed, delay of 5 minutes & then restart ###\n"));
  delay(5UL * 60UL * 1000UL);
  ESP.restart();
}



// setup & execute all device functions ...
void setup() {
  Serial.begin(74880);            // match the bootloader baud rate
  while (!Serial);  							// wait for serial to be initalised
  delay(2000);  									// give time to switch to the serial monitor
  Serial.println(F("\nSetup"));

  print_wakeup_reason();

  // restore the bootCount variables from RTC deep-sleep preserved RAM
  uint32_t address = 0;
  Serial.println(F("Recalling boot counts"));
  if(!ESP.rtcUserMemoryRead(address, &bootCount, sizeof(bootCount))) {
    Serial.println("RTC read failed!");
  }
  address += sizeof(bootCount);   // increment address for next read
  if(!ESP.rtcUserMemoryRead(address, &bootCountSinceUnsuccessfulJoin, sizeof(bootCountSinceUnsuccessfulJoin))) {
    Serial.println("RTC read failed!");
  }
  address += sizeof(bootCountSinceUnsuccessfulJoin);  // increment address for next read

  Serial.print(F("Boot count: "));
  Serial.println(++bootCount);

  int16_t state = 0;  						// return value for calls to RadioLib

  // setup the radio based on the pinmap (connections) in config.h
  Serial.println(F("Initalise the radio"));
  state = radio.begin();
  debug(state != RADIOLIB_ERR_NONE, F("Initalise radio failed"), state, true);

  Serial.println(F("Recalling LoRaWAN nonces & session"));
  // ##### setup the flash storage
  EEPROM.begin(RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  uint8_t LWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];										// create somewhere to store nonces
  for(uint8_t i = 0; i < RADIOLIB_LORAWAN_NONCES_BUF_SIZE; i++) {
    LWnonces[i] = EEPROM.read(i);
  }

  // if we have booted at least once we should have a session to restore, so report any failure
  // otherwise no point saying there's been a failure when it was bound to fail with an empty 
  // LWnonces var. At this point, bootCount has already been incremented, hence the > 2
  state = node.setBufferNonces(LWnonces); 															// send them to LoRaWAN
  debug((state != RADIOLIB_ERR_NONE) && (bootCount > 2), F("Restoring nonces buffer failed"), state, false);

  // recall session from RTC deep-sleep preserved variable
  if(!ESP.rtcUserMemoryRead(address, (uint32_t *)&LWsession, RADIOLIB_LORAWAN_SESSION_BUF_SIZE)) {
    Serial.println("RTC read failed!");
  }
  state = node.setBufferSession(LWsession); // send them to LoRaWAN stack
  // see comment above, no need to report a failure that is bound to occur on first boot
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
        uint8_t *persist = node.getBufferNonces();  // get pointer to nonces
        memcpy(LWnonces, persist, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);  // copy in to buffer
        for(uint8_t i = 0; i < RADIOLIB_LORAWAN_NONCES_BUF_SIZE; i++) {
          EEPROM.write(i, LWnonces[i]);
        }
        EEPROM.commit();

        // we'll save the session after the uplink

        // reset the failed join count
        bootCountSinceUnsuccessfulJoin = 0;

        delay(1000);  // hold off off hitting the airwaves again too soon - an issue in the US

    } // if beginOTAA state
  } // while join
  
  // ##### close the EEPROM
  EEPROM.end();
  

  // ----- and now for the main event -----
  Serial.println(F("Sending uplink"));

  // read some inputs
  uint8_t Digital2 = digitalRead(2);
  uint16_t Analog1 = analogRead(3);

  // build payload byte array
  uint8_t uplinkPayload[3];
  uplinkPayload[0] = Digital2;
  uplinkPayload[1] = highByte(Analog1);   // see notes for high/lowByte functions
  uplinkPayload[2] = lowByte(Analog1);
  
  // perform an uplink
  state = node.sendReceive(uplinkPayload, sizeof(uplinkPayload));    
  debug((state != RADIOLIB_LORAWAN_NO_DOWNLINK) && (state != RADIOLIB_ERR_NONE), F("Error in sendReceive"), state, false);

  Serial.print(F("FcntUp: "));
  Serial.println(node.getFcntUp());

  // now save session to RTC memory
  uint8_t *persist = node.getBufferSession();
  memcpy(LWsession, persist, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  if(!ESP.rtcUserMemoryWrite(address, (uint32_t *)&LWsession, RADIOLIB_LORAWAN_SESSION_BUF_SIZE)) {
    Serial.println("RTC write failed!");
  }
  
  // wait until next uplink - observing legal & TTN FUP constraints
  gotoSleep(uplinkIntervalSeconds);

}


// The ESP8266 wakes from deep-sleep and starts from the very beginning.
// It then goes back to sleep, so loop() is never called and which is
// why it is empty.

void loop() {}