/*
 * TrafficHelper.cpp
 * Copyright (C) 2019-2022 Linar Yusupov
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

#include <TimeLib.h>

#include "SoCHelper.h"
#include "TrafficHelper.h"
#include "NMEAHelper.h"
#include "EEPROMHelper.h"
#include "EPDHelper.h"

#include "SkyView.h"

traffic_t ThisAircraft, Container[MAX_TRACKING_OBJECTS], fo, EmptyFO;
traffic_by_dist_t traffic[MAX_TRACKING_OBJECTS];

static unsigned long UpdateTrafficTimeMarker = 0;
static unsigned long Traffic_Voice_TimeMarker = 0;

int max_alarm_level = ALARM_LEVEL_NONE;  /* global, used for visual displays */

void Traffic_Add()
{
    float fo_distance = fo.distance;

    if (fo_distance > ALARM_ZONE_NONE) {
        return;
    }

    if ( settings->filter == TRAFFIC_FILTER_OFF  ||
        (settings->filter == TRAFFIC_FILTER_500M &&
                      fo.RelativeVertical > -500 &&
                      fo.RelativeVertical <  500) ) {
      int i;

      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
        if (Container[i].ID == fo.ID) {
          uint8_t alert_bak = Container[i].alert;
          uint8_t alert_level = Container[i].alert_level;
          Container[i] = fo;
          Container[i].alert = alert_bak;
          Container[i].alert_level = alert_level;
          return;
        }
      }

      int max_dist_ndx = 0;
      int min_level_ndx = 0;
      float max_distance = 0;

      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {

        if (! Container[i].ID) {
            Container[i] = fo;
            return;
        }

        if (now() - Container[i].timestamp > ENTRY_EXPIRATION_TIME) {
            Container[i] = fo;
            return;
        }

        float distance = Container[i].distance;
        if  (distance > max_distance) {
          max_dist_ndx = i;
          max_distance = distance;
        }
        if (Container[i].alarm_level < Container[min_level_ndx].alarm_level)
            min_level_ndx = i;
      }

      if (fo.alarm_level > Container[min_level_ndx].alarm_level) {
        Container[min_level_ndx] = fo;
        return;
      }

      if (fo_distance <  max_distance &&
          fo.alarm_level  >= Container[max_dist_ndx].alarm_level) {
        Container[max_dist_ndx] = fo;
        return;
      }
    }
}

void Traffic_Update(traffic_t *fop)
{
  float distance, bearing;

  if (settings->protocol == PROTOCOL_GDL90) {

    distance = nmea.distanceBetween( ThisAircraft.latitude,
                                     ThisAircraft.longitude,
                                     fop->latitude,
                                     fop->longitude);

    bearing  = nmea.courseTo( ThisAircraft.latitude,
                              ThisAircraft.longitude,
                              fop->latitude,
                              fop->longitude);

    fop->RelativeNorth     = distance * cos(radians(bearing));
    fop->RelativeEast      = distance * sin(radians(bearing));
    fop->RelativeVertical  = fop->altitude - ThisAircraft.altitude;

    fop->RelativeBearing = bearing;
    fop->distance = distance;
    fop->adj_dist = fabs(distance) + VERTICAL_SLOPE * fabs(fop->RelativeVertical);

  } else if (fop->distance != 0) {    /* a PFLAU sentence: distance & bearing known */

    fop->adj_dist = fabs(fop->distance) + VERTICAL_SLOPE * fabs(fop->RelativeVertical);

  } else {           /* a PFLAA sentence */

    distance = sqrtf(fop->RelativeNorth * fop->RelativeNorth + fop->RelativeEast * fop->RelativeEast);
    fop->distance = distance;
    fop->adj_dist = fabs(distance) + VERTICAL_SLOPE * fabs(fop->RelativeVertical);

  }

  if (fop->alarm_level < fop->alert_level)     /* if gone farther then...   */
      fop->alert_level = fop->alarm_level;     /* ...alert if comes nearer again */
}

static void Traffic_Voice_One(traffic_t *fop)
{
    int bearing;
    char message[80];

    const char *u_dist, *u_alt;
    float voc_dist;
    int   voc_alt;
    const char *where;
    char how_far[32];
    char elev[32];

    if (settings->protocol == PROTOCOL_GDL90) {

        bearing = fop->RelativeBearing;

    } else if (fop->RelativeNorth == 0) {   // PFLAU

        bearing = fop->RelativeBearing;

    } else {   // PFLAA

        bearing = (int) (atan2f(fop->RelativeNorth,
                                fop->RelativeEast) * 180.0 / PI);  /* -180 ... 180 */
        /* convert from math angle into course relative to north */
        bearing = (bearing <= 90 ? 90 - bearing :
                                  450 - bearing);

        /* This bearing is always relative to current ground track */
//      if (settings->orientation == DIRECTION_TRACK_UP) {
            bearing -= ThisAircraft.Track;
//      }

    }

    if (bearing < 0) {
        bearing += 360;
    }

    int oclock = ((bearing + 15) % 360) / 30;

    switch (oclock)
    {
    case 0:
      where = "ahead";
      break;
    case 1:
      where = "1oclock";
      break;
    case 2:
      where = "2oclock";
      break;
    case 3:
      where = "3oclock";
      break;
    case 4:
      where = "4oclock";
      break;
    case 5:
      where = "5oclock";
      break;
    case 6:
      where = "6oclock";
      break;
    case 7:
      where = "7oclock";
      break;
    case 8:
      where = "8oclock";
      break;
    case 9:
      where = "9oclock";
      break;
    case 10:
      where = "10oclock";
      break;
    case 11:
      where = "11oclock";
      break;
    }

    switch (settings->units)
    {
    case UNITS_IMPERIAL:
      u_dist = "miles";   // "nautical miles";
      u_alt  = "feet";
      voc_dist = (fop->distance * _GPS_MILES_PER_METER) /
                  _GPS_MPH_PER_KNOT;
      voc_alt  = abs((int) (fop->RelativeVertical *
                  _GPS_FEET_PER_METER));
      break;
    case UNITS_MIXED:
      u_dist = "kms";
      u_alt  = "feet";
      voc_dist = fop->distance / 1000.0;
      voc_alt  = abs((int) (fop->RelativeVertical *
                  _GPS_FEET_PER_METER));
      break;
    case UNITS_METRIC:
    default:
      u_dist = "kms";
      u_alt  = "metres";
      voc_dist = fop->distance / 1000.0;
      voc_alt  = abs((int) fop->RelativeVertical);
      break;
    }

    if (voc_dist < 1.0) {
      strcpy(how_far, "near");
    } else {
      if (voc_dist > 9.0) {
        voc_dist = 9.0;
      }
      snprintf(how_far, sizeof(how_far), "%u %s", (int) voc_dist, u_dist);
    }

    if (voc_alt < 100) {
      strcpy(elev, "near");
    } else {
      if (voc_alt > 500) {
        voc_alt = 500;
      }

      snprintf(elev, sizeof(elev), "%u hundred %s %s",
        (voc_alt / 100), u_alt,
        fop->RelativeVertical > 0 ? "above" : "below");
    }

    snprintf(message, sizeof(message),
                "traffic %s distance %s altitude %s",
                where, how_far, elev);

    SoC->TTS(message);
}


static void Traffic_Voice()
{
  int i=0;
  int ntraffic=0;
  int bearing;
  char message[80];
  int sound_level_ndx = 0;
  max_alarm_level = ALARM_LEVEL_NONE;
  int sound_alarm_level = ALARM_LEVEL_NONE;    /* local, used for sound alerts */

  for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].ID) {

       if ((now() - Container[i].timestamp) <= VOICE_EXPIRATION_TIME) {

         /* find the maximum alarm level, whether to be alerted or not */
         if (Container[i].alarm_level > max_alarm_level) {
             max_alarm_level = Container[i].alarm_level;
         }

         /* figure out what is the highest alarm level needing a sound alert */
         if (Container[i].alarm_level > sound_alarm_level
                  && Container[i].alarm_level > Container[i].alert_level) {
             sound_alarm_level = Container[i].alarm_level;
             sound_level_ndx = i;
         }

         // traffic[ntraffic].fop = &Container[i];
         // traffic[ntraffic].distance = Container[i].distance;
         
         ntraffic++;
       }
    }
  }

//  if (ntraffic == 0) { return; }

// qsort(traffic, ntraffic, sizeof(traffic_by_dist_t), traffic_cmp_by_alarm);

  if (sound_alarm_level > ALARM_LEVEL_NONE) {
      traffic_t *fop = &Container[sound_level_ndx];
      Traffic_Voice_One(fop);
      fop->alert_level = sound_alarm_level;
         /* no more alerts for this aircraft at this alarm level */
      fop->timestamp = now();
  }

  /* do not issue voice alerts for non-alarm traffic */
}

void Traffic_setup()
{
  UpdateTrafficTimeMarker = millis();
  Traffic_Voice_TimeMarker = millis();
}

void Traffic_loop()
{
    if (isTimeToUpdateTraffic()) {

      for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {

        if (Container[i].ID &&
            (ThisAircraft.timestamp - Container[i].timestamp) <= ENTRY_EXPIRATION_TIME) {
          if ((ThisAircraft.timestamp - Container[i].timestamp) >= TRAFFIC_VECTOR_UPDATE_INTERVAL)
            Traffic_Update(&Container[i]);
        } else {
          Container[i] = EmptyFO;
        }
      }

      UpdateTrafficTimeMarker = millis();
    }

    if (isTimeToVoice()) {
        if (settings->voice != VOICE_OFF) {
            Traffic_Voice();
        Traffic_Voice_TimeMarker = millis();
    }

  }
}

void Traffic_ClearExpired()
{
  for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].ID && (now() - Container[i].timestamp) > ENTRY_EXPIRATION_TIME) {
      Container[i] = EmptyFO;
    }
  }
}

int Traffic_Count()
{
  int count = 0;

  for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].ID) {
      count++;
    }
  }

  return count;
}

/* still used by EPD_Draw_Text() */
int traffic_cmp_by_distance(const void *a, const void *b)
{
  traffic_by_dist_t *ta = (traffic_by_dist_t *)a;
  traffic_by_dist_t *tb = (traffic_by_dist_t *)b;

  if (ta->distance >  tb->distance) return  1;
  if (ta->distance <  tb->distance) return -1;
  return  0;
}

/* could have been used by Traffic_Voice() */
#if 0
int traffic_cmp_by_alarm(const void *a, const void *b)
{
  traffic_by_dist_t *ta = (traffic_by_dist_t *)a;
  traffic_by_dist_t *tb = (traffic_by_dist_t *)b;

  if (ta->fop->alarm_level >  tb->fop->alarm_level) return -1;   // sort descending
  if (ta->fop->alarm_level <  tb->fop->alarm_level) return  1;
  /* if same alarm level, then decide by distance (adjusted for altitude difference) */
  if (ta->fop->adj_dist >  tb->fop->adj_dist) return  1;
  if (ta->fop->adj_dist <  tb->fop->adj_dist) return -1;
  return  0;
}
#endif
