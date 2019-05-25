/* 
 * Uninterrruptible Power Supply (UPS) for a Raspberry Pi
 * 
 * This source file is part of the Raspberry Pi UPS Arduino firmware
 * found under http://www.github.com/microfarad-de/pi-ups
 * 
 * Please visit:
 *   http://www.microfarad.de
 *   http://www.github.com/microfarad-de
 * 
 * Copyright (C) 2019 Karim Hraibi (khraibi at gmail.com)
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>. 
 * 
 * Version: 1.0.0
 * Date:    May 2019
 */
#define VERSION_MAJOR 1  // major version
#define VERSION_MINOR 0  // minor version
#define VERSION_MAINT 0  // maintenance version

#include <avr/wdt.h>
#include <Arduino.h>
#include "Helper.h"
#include "LiCharger.h"
#include "Cli.h"
#include "ADC.h"
#include "Led.h"



/*
 * Pin assignment
 */
#define NUM_APINS                4  // Number of analog pins in use
#define V_IN_APIN         ADC_PIN0  // Analog pin for measuring V_in
#define V_BATT_APIN       ADC_PIN2  // Analog pin for measuring V_batt
#define V_UPS_APIN        ADC_PIN3  // Analog pin for measuring V_ups
#define CHG_MOSFET_PIN           3  // Digital pin with PWM support for controlling the MOSFET gate of the battery charging circuit
#define BATT_MOSFET_PIN          5  // Digital pin controlling the MOSFET gate for switching the battery power
#define IN_MOSFET_PIN            7  // Digital pin controlling the MOSFET gate for switching the external power
#define OUT_MOSFET_PIN           9  // Digital pin controlling the MOSFET gate for switching the output
#define LED_PIN                  2  // LED digital pin



/*
 * Configuration parameters
 */
#define SERIAL_BAUD          115200   // Serial communication baud rate
#define I_CHRG                  500   // 500mA - Constant charging current in mA
#define ADC_AVG_SAMPLES          16   // Number of ADC samples to be averaged
#define V_IN_THR_BATTERY    4900000   // 4.85 V - V_in thershold in µV below which the UPS will switch to battery power
#define V_IN_THR_EXTERNAL   5000000   // 5.0 V - V_in threshold in µV above which the UPS will switch back to external power
#define V_BATT_THR_75       3800000   // 3.8 V - V_batt threshold in µV that roughly corresponds to 75% battery charge
#define V_BATT_THR_50       3600000   // 3.6 V - V_batt threshold in µV that roughly corresponds to 50% battery charge        
#define V_BATT_THR_25       3400000   // 3.4 V - V_batt threshold in µV that roughly corresponds to 25% battery charge
#define V_BATT_THR_LOW      3200000   // 3.2. V - V_batt threshold in µV for initiating a system shutdown
#define V_BATT_THR_ERROR    1000000   // 1.0 V - V_batt threshold in µV for signalling a battery error
#define INITIAL_DELAY          5000   // Initial power on delay in ms
#define EXTERNAL_DELAY         1000   // Delay in ms prior to switching back to external power
#define SHUTDOWN_DELAY        60000   // Delay in ms prior to turning off power upon system shutdown
#define RESTART_DELAY          5000   // Delay in ms prior to restarting the system following a shutdown




/* 
 * Objects
 */
LedClass Led;
LiChargerClass LiCharger;

/*
 * State machine states
 */
enum State_t { STATE_INIT_E, STATE_INIT, STATE_EXTERNAL_E, STATE_EXTERNAL, STATE_BATTERY_E, STATE_BATTERY, 
                 STATE_CALIBRATE_E, STATE_CALIBRATE, STATE_ERROR_E, STATE_ERROR };

/*
 * A list of error codes
 * Each code sets a different bit. Codes can be combined via addition.
 */
enum Error_t {
  ERROR_NONE    = 0,   // No errors
  ERROR_BATTERY = 1,   // Battery error
  ERROR_CRC     = 2    // CRC error
};


/*
 * Battery states
 */
enum BattState_t {
  BATT_STATE_0   = 0,    // Low battery
  BATT_STATE_25  = 25,   // 25%
  BATT_STATE_50  = 50,   // 50%
  BATT_STATE_75  = 75,   // 75%
  BATT_STATE_100 = 100   // 100%
};

/*
 * Global variables
 */
struct {
  State_t state = STATE_INIT_E; // Current state machine state
  uint32_t vIn;          // V_in - external power supply voltage in µV
  uint32_t vUps;         // V_ups - voltage at the output of the DC-DC converter in µV
  uint32_t vBatt;        // V_batt - Battery voltage in µV
  uint64_t iBatt;        // I_batt - Battery charging current in µA
  uint16_t vInRaw;       // Raw ADC value of V_in
  uint16_t vUpsRaw;      // Raw ADC value of V_ups
  uint16_t vBattRaw;     // Raw ADC value of V_batt 
  BattState_t battState; // Battery st      
  uint8_t error = 0;     // Error code
  bool shutdown = false; // System shutdown command
  char *stateStr = 0;    // System status as human readable string
} G;



/*
 * Parameters stored in EEPROM (non-volatile memory)
 */
struct {
  uint32_t vInCal;       // V_in_cal - Calibration constant for calculating V_in
  uint32_t vUpsCal;      // V_ups_cal - Calibration constant for calculating V_ups
  uint32_t vBattCal;     // V_batt_cal - Calibration constant for calculating V_batt
  uint16_t rShunt;       // R_shunt - Shunt resistor value in mΩ
  uint16_t vDiode;       // V_diode - charger diode voltage drop in mV
  uint32_t crc;          // CRC checksum
} Nvm;



/*
 * Strings to be reused for saving memory
 */
const struct {
  char *R_shunt    = (char *)"R_shunt    = %u mΩ\n";
  char *V_diode    = (char *)"V_diode    = %u mV\n";
  char *V_in_cal   = (char *)"V_in_cal   = %lu\n";
  char *V_ups_cal  = (char *)"V_ups_cal  = %lu\n";
  char *V_batt_cal = (char *)"V_batt_cal = %lu\n";
  char *CRC        = (char *)"CRC        = %lx\n";
  char *EXTERN     = (char *)"EXTERNAL";
  char *BATTERY    = (char *)"BATTERY %u";
  char *SHUTDOWN   = (char *)"SHUTDOWN %u";
  char *CALIBRATE  = (char *)"CALIBRATE";
  char *ERR        = (char *)"ERROR %u";
} Str;


/*
 * Arduino initalization routine
 */
void setup (void) {

  MCUSR = 0;      // clear MCU status register
  wdt_disable (); // and disable watchdog

  // Initialize pins
  pinMode (CHG_MOSFET_PIN, OUTPUT);
  pinMode (IN_MOSFET_PIN, OUTPUT);
  pinMode (OUT_MOSFET_PIN, OUTPUT);
  pinMode (BATT_MOSFET_PIN, OUTPUT);
  analogWrite (CHG_MOSFET_PIN, 255);     // Active low: max duty cycle means the MOSFET is off
  digitalWrite (IN_MOSFET_PIN, LOW);     // Active low: LOW means the MOSFET is on
  digitalWrite (OUT_MOSFET_PIN, HIGH);   // Active low: HIGH means the MOSFET is off
  digitalWrite (BATT_MOSFET_PIN, HIGH);  // Active low: HIGH means the MOSFET is off
  
  
  // Initialize the command-line interface
  Cli.init ( SERIAL_BAUD );
  Cli.xputs ("");
  Cli.xputs ("+ + +  P I  U P S  + + +");
  Cli.xputs ("");
  Cli.xprintf ("V %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_MAINT);
  Cli.xputs ("");
  Cli.newCmd ("stat", "Brief status", cmdStat);
  Cli.newCmd ("s", "", cmdStat);
  Cli.newCmd ("status", "Detaild status", cmdStatus);
  Cli.newCmd (".", "", cmdStatus);
  Cli.newCmd ("e", "EEPROM status", cmdEEPROM);
  Cli.newCmd ("halt", "Initiate shutdown", cmdHalt);
  Cli.newCmd ("clear", "Clear error codes", cmdClear);
  Cli.newCmd ("cal", "Calibrate (arg: [vin|vups|vbatt])", cmdCal);
  Cli.newCmd ("rshunt", "Set R_shunt in mΩ", cmdRshunt);
  Cli.newCmd ("vdiode", "Set V_diode in mV", cmdVdiode);
  Cli.showHelp ();

  // Initialize the ADC
  ADConv.initialize (ADC_PRESCALER_128, ADC_INTERNAL, NUM_APINS, ADC_AVG_SAMPLES);

  // Initialize the battery charger
  LiCharger.initialize (1, I_CHRG, liChargerCB);

  // Initialize the LED
  Led.initialize (LED_PIN);

  // Read the settings from EEPROM
  nvmRead ();

  // Enable the watchdog
  wdt_enable (WDTO_1S);
}


/*
 * Arduino main loop
 */
void loop (void) {
  static uint32_t delayTs;  // Timestamp for measuring delays
  uint32_t ts = millis ();  // General purpose millisecond timestamp

  // Reset the watchdog timer
  wdt_reset ();

  // Command-line interpreter
  Cli.getCmd ();

  // Update the LED state
  Led.loopHandler ();

  // Read the ADC channels
  adcRead ();

  // Update the battery charger state
  LiCharger.loopHandler (G.vBatt, G.iBatt);

  // Check the battery state
  checkBattState ();

  // Handle shutdown command
  shutdown ();
 
  // Main state machine
  switch (G.state) {

    case STATE_INIT_E:
      G.shutdown = false;
      delayTs = ts;
      G.state = STATE_INIT;
    case STATE_INIT:
      // Wait for the ADC to stabilize before starting-up
      if (ts - delayTs > (uint32_t)INITIAL_DELAY) {
        digitalWrite (OUT_MOSFET_PIN, LOW);   // Activate output power
        G.state = STATE_EXTERNAL_E;
      }
      break;
      
    case STATE_EXTERNAL_E:
      LiCharger.start ();                   // Start battery charging
      digitalWrite (IN_MOSFET_PIN, LOW);    // Activate external power
      digitalWrite (BATT_MOSFET_PIN, HIGH); // Deactivate battery power
      G.stateStr = Str.EXTERN;
      Cli.xputs(G.stateStr);
      G.state = STATE_EXTERNAL;
    case STATE_EXTERNAL:

      if (!G.shutdown) {
        Led.blink (-1, 100, 1900);
      }
      // Switch to battery power if V_in is below the specified threshold
      if (G.vIn < (uint32_t)V_IN_THR_BATTERY) {
        G.state = STATE_BATTERY_E;
      }
      break;

    case STATE_BATTERY_E:
      LiCharger.stop ();                   // Stop battery charging
      digitalWrite (BATT_MOSFET_PIN, LOW); // Activate battery power
      digitalWrite (IN_MOSFET_PIN, HIGH);  // Deactivate external power
      delayTs = ts;
      G.stateStr = Str.BATTERY;
      Cli.xprintf (G.stateStr, G.battState);
      Cli.xputs ("");
      G.state = STATE_BATTERY;
    case STATE_BATTERY:

      if (!G.shutdown) {
        // Adapt the LED blinking duty cycle according to batter voltage
        if      (G.battState == BATT_STATE_0)  Led.blink (-1, 100, 100);
        else if (G.battState == BATT_STATE_25) Led.blink (-1, 250, 750);
        else if (G.battState == BATT_STATE_50) Led.blink (-1, 500, 500);
        else if (G.battState == BATT_STATE_75) Led.blink (-1, 750, 250);
        else                                   Led.blink (-1, 1000, 0);
      }

      // Switch back to external power if V_in is above the specified threshold during EXTERNAL_DELAY
      if (G.vIn < (uint32_t) V_IN_THR_EXTERNAL) delayTs = ts;
      if (ts - delayTs > EXTERNAL_DELAY) G.state = STATE_EXTERNAL_E;
      break;

    case STATE_CALIBRATE_E:
      Led.blink (-1, 500, 1500);
      LiCharger.stop ();                    // Stop battery charging
      digitalWrite (OUT_MOSFET_PIN, LOW);   // Activate output power
      digitalWrite (IN_MOSFET_PIN, LOW);    // Activate external power
      digitalWrite (BATT_MOSFET_PIN, HIGH); // Deactivate battery power
      G.shutdown = false;
      G.stateStr = Str.CALIBRATE;
      Cli.xputs(G.stateStr);
      G.state = STATE_CALIBRATE;
    case STATE_CALIBRATE:
      // Do nothing and wait for a CLI command
      break;

    case STATE_ERROR_E:
      LiCharger.stop ();                    // Stop battery charging
      digitalWrite (IN_MOSFET_PIN, LOW);    // Activate external power
      digitalWrite (BATT_MOSFET_PIN, HIGH); // Deactivate battery power
      G.stateStr = Str.ERR;
      Cli.xprintf(G.stateStr, G.error);
      Cli.xputs ("");
      G.state = STATE_ERROR;
    case STATE_ERROR:
      if (!G.shutdown) {
        Led.blink (-1, 200, 200);
      }
      break;

    default:
      break;
    
  }

}



/*
 * Shutdown routine
 */
void shutdown (void) {
  static uint32_t shutdownTs;
  uint32_t ts = millis ();

  if (G.shutdown) {
    Led.blink (-1, 50, 50);
    
    if  (digitalRead (OUT_MOSFET_PIN) == LOW) {  
    
      // Power down if HALT_DELAY has elapsed
      if (ts - shutdownTs > SHUTDOWN_DELAY) {
        digitalWrite (OUT_MOSFET_PIN, HIGH);  // Deactivate output power
        shutdownTs = ts;
      }
    }
    else {
      
      // Power up if RESTART_DELAY has elapsed and not on battery power
      if (ts - shutdownTs > RESTART_DELAY && G.state != STATE_BATTERY) {
        digitalWrite (OUT_MOSFET_PIN, LOW);  // Activate output power
        G.shutdown = false;
      } 
    }    
  }
  else {
    shutdownTs = ts;
  }      
}



/*
 * Callback function in use by the battery charger
 * for setting the PWM value
 */
void liChargerCB (uint8_t pwm) {

  // Adjust the PWM value. The pin is active low, thus, we need to invert the PWM value.
  analogWrite (CHG_MOSFET_PIN, 255 - pwm);
}



/*
 * Read the ADC channels
 */
void adcRead (void) {
  bool result;

  // Read the ADC channels
  result = ADConv.readAll ();

  
  if (result) {
    // Get the ADC results
    G.vInRaw   = (uint16_t)ADConv.result[V_IN_APIN];
    G.vUpsRaw  = (uint16_t)ADConv.result[V_UPS_APIN];
    G.vBattRaw = (uint16_t)ADConv.result[V_BATT_APIN];

    // Calculate voltage and current
    G.vIn   = (uint32_t)G.vInRaw * Nvm.vInCal;
    G.vUps  = (uint32_t)G.vUpsRaw * Nvm.vUpsCal;
    G.vBatt = (uint32_t)G.vBattRaw * Nvm.vBattCal;
    G.iBatt = (( (uint64_t)G.vIn - (uint64_t)G.vBatt - (uint64_t)Nvm.vDiode*1000) * LiCharger.pwm * 1000) / 255 / Nvm.rShunt ;
    if (G.iBatt < 0) G.iBatt = 0;

  }
}




/*
 * Validate the settings
 * Called after reading or before writing EEPROM
 * Always fall-back to the safest possible values
 */
void nvmValidate (void) {
  if (Nvm.vInCal < 4000 || Nvm.vInCal > 40000) Nvm.vInCal = 40000;
  if (Nvm.vUpsCal < 4000 || Nvm.vUpsCal > 40000) Nvm.vUpsCal = 40000;
  if (Nvm.vBattCal < 4000 || Nvm.vBattCal > 40000) Nvm.vBattCal = 40000;
  if (Nvm.rShunt < 100 || Nvm.rShunt > 1000) Nvm.rShunt = 1000;
  if (Nvm.vDiode < 100 || Nvm.vDiode > 1000) Nvm.vDiode = 100;
}


/*
 * Read and validate EEPROM data
 */
void nvmRead (void) {
  uint32_t crc;
  
  eepromRead (0x0, (uint8_t*)&Nvm, sizeof (Nvm)); 
  nvmValidate ();

  // Calculate and check CRC checksum
  crc = crcCalc ((uint8_t*)&Nvm, sizeof (Nvm) - sizeof (Nvm.crc) );
  Cli.xputs ("");
  Cli.xprintf (Str.CRC, crc);
  
  if (crc != Nvm.crc) {
    Cli.xputs ("CRC error");
    G.error = G.error | ERROR_CRC;
    G.state = STATE_ERROR_E;
  }
}


/*
 * Write and validate EEPROM data
 */
void nvmWrite (void) {
  nvmValidate (); 
  Nvm.crc = crcCalc ((uint8_t*)&Nvm, sizeof (Nvm) - sizeof (Nvm.crc) );
  eepromWrite (0x0, (uint8_t*)&Nvm, sizeof (Nvm));
}


/*
 * Check the battery state
 */
void checkBattState (void) {
  // Check the battery voltage
  if      (G.vBatt < (uint32_t)V_BATT_THR_LOW) G.battState = BATT_STATE_0;
  else if (G.vBatt < (uint32_t)V_BATT_THR_25)  G.battState = BATT_STATE_25;
  else if (G.vBatt < (uint32_t)V_BATT_THR_50)  G.battState = BATT_STATE_50;
  else if (G.vBatt < (uint32_t)V_BATT_THR_75)  G.battState = BATT_STATE_75;
  else                                         G.battState = BATT_STATE_100;

  // Check for battery error
  if (G.state != STATE_INIT_E && G.state != STATE_INIT && 
        G.vBatt < V_BATT_THR_ERROR && (G.error & (uint8_t)ERROR_BATTERY) == 0) {
    G.error = G.error | ERROR_BATTERY;
    G.state = STATE_ERROR_E;
  }
}



/*
 * CLI command reporting the brief system status
 */
int cmdStat (int argc, char **argv) {
  if (G.shutdown)  {
    Cli.xprintf (Str.SHUTDOWN, digitalRead (OUT_MOSFET_PIN));
  }
  else if (G.state == STATE_BATTERY) {
    Cli.xprintf (G.stateStr, G.battState);   
  }
  else if (G.state == STATE_ERROR) {
    Cli.xprintf (G.stateStr, G.error);
  }
  else {
    Cli.xprintf (G.stateStr);
  }
  Cli.xputs("");
  return 0;
}


/*
 * CLI command for initiating a system shutdown
 */
int cmdHalt (int argc, char **argv) {
  if (G.state != STATE_CALIBRATE_E && G.state != STATE_CALIBRATE) {
    Cli.xprintf (Str.SHUTDOWN, digitalRead (OUT_MOSFET_PIN));
    Cli.xputs ("");
    G.shutdown = true; 
  }
  return 0;
}

/*
 * CLI command for clearing all the error codes
 */
int cmdClear (int argc, char **argv) {
  Cli.xputs ("cleared");
  G.error = ERROR_NONE;
  G.state = STATE_EXTERNAL_E;
  return 0;
}

/*
 * CLI command for showing the detailed system status
 */
int cmdStatus (int argc, char **argv) {
  Cli.xputs ("");
  Cli.xprintf ("state      = ");
  if (G.state == STATE_BATTERY) {
    Cli.xprintf (G.stateStr, G.battState);   
  }
  else if (G.state == STATE_ERROR) {
    Cli.xprintf (G.stateStr, G.error);
  }
  else {
    Cli.xprintf (G.stateStr);
  }
  if (G.shutdown)  {
    Cli.xprintf (" ");
    Cli.xprintf (Str.SHUTDOWN, digitalRead (OUT_MOSFET_PIN));
  }
  Cli.xputs ("");
  Cli.xprintf ("battery    = %u\n", G.battState);
  Cli.xprintf ("V_in       = %lu mV\n", G.vIn / 1000);
  Cli.xprintf ("V_ups      = %lu mV\n", G.vUps / 1000);
  Cli.xprintf ("V_batt     = %lu mV\n", G.vBatt / 1000);
  Cli.xprintf ("I_batt     = %lu mA\n", G.iBatt / 1000);
  Cli.xprintf ("PWM        = %u\n", LiCharger.pwm);
  Cli.xprintf ("V_in_raw   = %u\n", G.vInRaw);
  Cli.xprintf ("V_ups_raw  = %u\n", G.vUpsRaw);
  Cli.xprintf ("V_batt_raw = %u\n", G.vBattRaw);

  return 0;
}



/*
 * CLI command for displaying the EEPROM settings
 */
int cmdEEPROM (int argc, char **argv) {
  Cli.xputs ("");
  Cli.xprintf (Str.V_in_cal,   Nvm.vInCal);
  Cli.xprintf (Str.V_ups_cal,  Nvm.vUpsCal);
  Cli.xprintf (Str.V_batt_cal, Nvm.vBattCal);
  Cli.xprintf (Str.R_shunt,    Nvm.rShunt);
  Cli.xprintf (Str.V_diode,    Nvm.vDiode);
  Cli.xprintf (Str.CRC,        Nvm.crc);
  return 0;
}


/*
 * CLI command for setting the shunt resistor value
 * argv[1]: shunt resistance in mΩ
 */
int cmdRshunt (int argc, char **argv) {
  Nvm.rShunt = atoi (argv[1]);
  nvmWrite ();
  Cli.xprintf(Str.R_shunt, Nvm.rShunt);
  return 0;
}


/*
 * CLI command for setting the charger diode voltage drop
 * argv[1]: shunt voltage in mV
 */
int cmdVdiode (int argc, char **argv) {
  Nvm.vDiode = atoi (argv[1]);
  nvmWrite ();
  Cli.xprintf(Str.V_diode, Nvm.vDiode);
  return 0;
}



/*
 * CLI command for calibrating V_in, V_ups and V_batt
 * argv[1]:
 *   vin   : calibrate V_in
 *   vups  : calibrate V_ups
 *   vbatt : calibrate V_batt
 * 
 * argv[2]: 
 *   Measured reference voltage in mV
 */
int cmdCal (int argc, char **argv) {
  if (G.state == STATE_CALIBRATE) {
    uint32_t vRef = (uint32_t)atoi(argv[2]) * 1000; 
    if      (strcmp(argv[1], "vin"  ) == 0) calVin (vRef);
    else if (strcmp(argv[1], "vups" ) == 0) calVups (vRef);
    else if (strcmp(argv[1], "vbatt") == 0) calVbatt (vRef);
    else    G.state = STATE_EXTERNAL_E, Cli.xputs ("Cal. mode end");
  }
  else {
    G.state = STATE_CALIBRATE_E;
    Cli.xputs ("Cal. mode begin");        
  }
  return 0;
}


/*
 * Calibrate V_in
 */
void calVin (uint32_t vRef) {
  Nvm.vInCal = (uint32_t)vRef / (uint32_t)G.vInRaw;
  nvmWrite ();
  Cli.xprintf (Str.V_in_cal, Nvm.vInCal);
}


/*
 * Calibrate V_ups
 */
void calVups (uint32_t vRef) {
  Nvm.vUpsCal = (uint32_t)vRef / (uint32_t)G.vUpsRaw;
  nvmWrite ();
  Cli.xprintf (Str.V_ups_cal, Nvm.vUpsCal);
}


/*
 * Calibrate V_batt
 */
void calVbatt (uint32_t vRef) {
  Nvm.vBattCal = (uint32_t)vRef / (uint32_t)G.vBattRaw;
  nvmWrite ();
  Cli.xprintf (Str.V_batt_cal, Nvm.vBattCal);
}
