/*
 * BatteryHelper.cpp
 * Copyright (C) 2016-2021 Linar Yusupov
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
 */

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include "../system/SoC.h"
#include "EEPROM.h"
#include "Battery.h"

unsigned long Battery_TimeMarker        = 0;

static float Battery_voltage_cache      = 0;
static int Battery_cutoff_count         = 0;

void Battery_setup()
{
  SoC->Battery_setup();

  Battery_voltage_cache = SoC->Battery_param(BATTERY_PARAM_VOLTAGE);
  Battery_TimeMarker = millis();
}

float Battery_voltage()
{
  return Battery_voltage_cache;
}

/* low battery voltage threshold */
float Battery_threshold()
{
  return SoC->Battery_param(BATTERY_PARAM_THRESHOLD);
}

/* Battery is empty */
float Battery_cutoff()
{
  return SoC->Battery_param(BATTERY_PARAM_CUTOFF);
}

/* Battery charge level (in %) */
uint8_t Battery_charge() {
  return (uint8_t) SoC->Battery_param(BATTERY_PARAM_CHARGE);
}

/*
 * When set to run on external power but with a battery installed, allow running
 * on the battery as long as still airborne.  Shut down after at least an hour
 * of operation, once external power is turned off, and battery voltage is
 * somewhat down.  For now only implemented for T-Beam.
 */
static bool follow_ext_power_shutoff(float voltage)
{
#if defined(ESP32)
    if (! settings->power_external)
        return false;
    if (hw_info.model != SOFTRF_MODEL_PRIME_MK2)
        return false;
    if (ESP32_onExternalPower())
        return false;
    if (ThisAircraft.airborne)
        return false;
    if (voltage >= 3.9)
        return false;
    if (millis() < 3600000)
        return false;
    return true;
#else
    return false;
#endif
}

void Battery_loop()
{
  if (isTimeToBattery()) {
    float voltage = SoC->Battery_param(BATTERY_PARAM_VOLTAGE);

    if (voltage > BATTERY_THRESHOLD_INVALID &&
         (voltage < Battery_cutoff() || follow_ext_power_shutoff(voltage))) {
      if (Battery_cutoff_count > 3) {
        shutdown(SOFTRF_SHUTDOWN_LOWBAT);
      } else {
        Battery_cutoff_count++;
      }
    } else {
      Battery_cutoff_count = 0;
    }

    Battery_voltage_cache = voltage;
    Battery_TimeMarker = millis();
  }
}
