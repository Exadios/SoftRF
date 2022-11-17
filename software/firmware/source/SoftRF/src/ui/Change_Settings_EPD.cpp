/*
 * Change_Settings.cpp
 * Copyright (C) 2022 Moshe Braner
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

#include "../system/SoC.h"

#if defined(USE_EPAPER)

#include "../driver/EPD.h"

#include <TimeLib.h>

#include "../TrafficHelper.h"
#include "../driver/EEPROM.h"
#include "../protocol/data/NMEA.h"
#include "../protocol/data/GDL90.h"
#include "../driver/GNSS.h"
#include "../driver/LED.h"
#include "../driver/RF.h"

#include <protocol.h>
#include "../protocol/radio/Legacy.h"

#include <gfxfont.h>
#include <FreeMonoBold12pt7b.h>


struct set_entry
{
    int code;
    const char *label;
};

set_entry actypes[] = {
  {AIRCRAFT_TYPE_GLIDER,     "Glider"},
  {AIRCRAFT_TYPE_TOWPLANE,   "Towplane"},
  {AIRCRAFT_TYPE_HELICOPTER, "Helicopter"},
  {AIRCRAFT_TYPE_POWERED,    "Powered"},
  {AIRCRAFT_TYPE_HANGGLIDER, "Hangglider"},
  {AIRCRAFT_TYPE_PARAGLIDER, "Paraglider"},
  {AIRCRAFT_TYPE_DROPPLANE,  "Dropplane"},
  {AIRCRAFT_TYPE_PARACHUTE,  "Parachute"},
  {AIRCRAFT_TYPE_BALLOON,    "Balloon"},
  {AIRCRAFT_TYPE_UAV,        "UAV"},
  {AIRCRAFT_TYPE_STATIC,     "Static"},
  {-1, NULL}
};

set_entry protocols[] = {
  {RF_PROTOCOL_LEGACY,    "LEGACY"},
  {RF_PROTOCOL_OGNTP,     "OGNTP"},
  {RF_PROTOCOL_P3I,       "P3I"},
  {RF_PROTOCOL_FANET,     "FANET"},
//  {RF_PROTOCOL_ADSB_UAT,  "ADSB-UAT"},
//  {RF_PROTOCOL_ADSB_1090, "ADSB-1090"},
  {-1, NULL}
};

set_entry regions[] = {
  {RF_BAND_EU, "EU"},
  {RF_BAND_US, "US"},
  {RF_BAND_UK, "UK"},
  {RF_BAND_AU, "AU"},
  {RF_BAND_NZ, "NZ"},
  {RF_BAND_RU, "RU"},
  {RF_BAND_CN, "CN"},
  {RF_BAND_IN, "IN"},
  {RF_BAND_IL, "IL"},
  {RF_BAND_KR, "KR"},
  {-1, NULL}
};

set_entry alarms[] = {
  {TRAFFIC_ALARM_LEGACY,   "Legacy"},
  {TRAFFIC_ALARM_VECTOR,   "Vector"},
  {TRAFFIC_ALARM_DISTANCE, "Distance"},
  {TRAFFIC_ALARM_NONE,     "None"},
  {-1, NULL}
};

set_entry units[] = {
  {UNITS_METRIC,   "Metric"},
  {UNITS_IMPERIAL, "Imperial"},
  {UNITS_MIXED,    "Mixed"},
  {-1, NULL}
};

set_entry directions[] = {
  {DIRECTION_TRACK_UP, "Track Up"},
  {DIRECTION_NORTH_UP, "North Up"},
  {-1, NULL}
};

enum
{
	DECISION_CANCEL = 0,
	DECISION_REVIEW,
	DECISION_SAVE
};

set_entry decisions[] = {
  {DECISION_CANCEL, "cancel"},
  {DECISION_REVIEW, "review"},
  {DECISION_SAVE,  "! SAVE !"},
  {-1, NULL}
};

static int decision = 0;
static int actype = 0;
static int protocol = 0;
static int region = 0;
static int alarm = 0;
static int unit = 0;
static int direction = 0;

/* search for a given code and return the index */
int get_one_setting(int setting, set_entry *list)
{
    int i = 0;
    while (list[i].code != setting) {
        ++i;
        if (list[i].code < 0)
            return 0;
    }
    return i;
}

struct page {
    int *indexvar;
    set_entry *options;
    const char *line1;
    const char *line2;
    const char *line3;    
};

page pages[] = {
  {&decision, decisions, NULL, "what to", "do next:"},
  {&actype, actypes, NULL, "Aircraft", "Type:"},
  {&protocol, protocols, NULL, "RF", "Protocol:"},
  {&region, regions, NULL, "Frequency", "Band:"},
  {&alarm, alarms, "Collision", "Prediction", "Algorithm:"},
  {&unit, units, NULL, "Display", "Units:"},
  {&direction, directions, NULL, "Display", "Orientation:"},
  {NULL, NULL, NULL, NULL, NULL}
};

int curpage = 0;

void EPD_chgconf_next() { }

static bool chgconf_initialized = false;

void get_settings()
{
    if (chgconf_initialized)
        return;
    actype    = get_one_setting((int) settings->aircraft_type, actypes);
    protocol  = get_one_setting((int) settings->rf_protocol, protocols);
    region    = get_one_setting((int) settings->band, regions);
    alarm     = get_one_setting((int) settings->alarm, alarms);
    unit      = get_one_setting((int) ui->units, units);
    direction = get_one_setting((int) ui->orientation, directions);
    decision = 0;  // cancel
    curpage = 1;   // actypes
    chgconf_initialized = true;
}

void EPD_chgconf_save()
{
    settings->aircraft_type = actypes[actype].code;
    settings->rf_protocol   = protocols[protocol].code;
    settings->band          = regions[region].code;
    settings->alarm         = alarms[alarm].code;
    ui->units               = units[unit].code;
    ui->orientation         = directions[direction].code;
    SoC->WDT_fini();
    if (SoC->Bluetooth_ops) { SoC->Bluetooth_ops->fini(); }
    EEPROM_store();
}

/*
 * Scroll to the next page, i.e., next item to be adjusted.
 * Tied to the Mode button.
 */
void EPD_chgconf_page()
{
    if (EPD_view_mode != VIEW_CHANGE_SETTINGS)
        return;
    if (curpage == 0) {  // pages[curpage].indexvar == &decision
        if (decision == DECISION_CANCEL) {
            chgconf_initialized = false;
            // EPD_prev_view = VIEW_CHANGE_SETTINGS;
            EPD_view_mode = VIEW_MODE_CONF;
            conf_initialized = false;
            return;
        }
        if (decision == DECISION_SAVE) {
            EPD_view_mode = VIEW_SAVE_SETTINGS;
            return;
        }
    }
    ++curpage;
    if (pages[curpage].indexvar == NULL)
        curpage = 0;
//Serial.print("curpage: ");
//Serial.println(curpage);
}

/*
 * Scroll to the next value available for this item.
 * Tied to the Touch button.
 */
void EPD_chgconf_prev()
{
    if (EPD_view_mode != VIEW_CHANGE_SETTINGS)
        return;
    int i = *(pages[curpage].indexvar) + 1;
    if (pages[curpage].options[i].code < 0)
        i = 0;
    *(pages[curpage].indexvar) = i;
//Serial.print("index: ");
//Serial.println(i);
}

static void EPD_Draw_chgconf()
{

#if defined(USE_EPD_TASK)
    if (EPD_update_in_progress != EPD_UPDATE_NONE)
        return;
#endif

    display->setFont(&FreeMonoBold12pt7b);

      uint16_t x = 4;
      uint16_t y = 20;

      int16_t  tbx, tby;
      uint16_t tbw, tbh;

      display->fillScreen(GxEPD_WHITE);

      Serial.println();

      const char *line;

      line = pages[curpage].line1;
      if (line == NULL) {
          display->getTextBounds("dummy", 0, 0, &tbx, &tby, &tbw, &tbh);
          y += tbh;
      } else {
          display->getTextBounds(line, 0, 0, &tbx, &tby, &tbw, &tbh);
          y += tbh;
          display->setCursor(x, y);
          display->print(line);
          Serial.println(line);
      }
      y += TEXT_VIEW_LINE_SPACING;

      line = pages[curpage].line2;
      display->getTextBounds(line, 0, 0, &tbx, &tby, &tbw, &tbh);
      y += tbh;
      display->setCursor(x, y);
      display->print(line);
      Serial.println(line);

      y += TEXT_VIEW_LINE_SPACING;

      line = pages[curpage].line3;
      display->getTextBounds(line, 0, 0, &tbx, &tby, &tbw, &tbh);
      y += tbh;
      display->setCursor(x, y);
      display->print(line);
      Serial.println(line);

      y += TEXT_VIEW_LINE_SPACING + 20;
      x += 20;

      line = pages[curpage].options[*(pages[curpage].indexvar)].label;
      display->getTextBounds(line, 0, 0, &tbx, &tby, &tbw, &tbh);
      y += tbh;
      display->setCursor(x, y);
      display->print(line);
      Serial.println(line);

      Serial.println();

#if defined(USE_EPD_TASK)
    /* a signal to background EPD update task */
    EPD_update_in_progress = EPD_UPDATE_FAST;
#else
    display->display(true);
#endif
}

void EPD_chgconf_loop()
{
  if (isTimeToEPD()) {
      EPDTimeMarker = millis();

      if (EPD_view_mode == VIEW_CHANGE_SETTINGS) {
          get_settings();
          EPD_Draw_chgconf();

      } else if (EPD_view_mode == VIEW_SAVE_SETTINGS) {
          Serial.println("SAVING SETTINGS...");
          EPD_chgconf_save();
          EPD_Message("SETTINGS", "SAVED");
          Serial.println("...SETTINGS SAVED");
          EPD_view_mode = VIEW_REBOOT;

      } else if (EPD_view_mode == VIEW_REBOOT) {
          Serial.println("NOW REBOOTING");
          reboot();
          Serial.println(F("This will never be printed."));
      }
  }
}

#endif /* USE_EPAPER */
