/*
 * TrafficHelper.cpp
 * Copyright (C) 2018-2021 Linar Yusupov
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

#include "TrafficHelper.h"
#include "Wind.h"
#include "driver/EEPROM.h"
#include "driver/RF.h"
#include "driver/GNSS.h"
#include "driver/Sound.h"
#include "ui/Web.h"
#include "protocol/radio/Legacy.h"
#include "ApproxMath.h"

unsigned long UpdateTrafficTimeMarker = 0;

ufo_t fo, Container[MAX_TRACKING_OBJECTS], EmptyFO;
traffic_by_dist_t traffic_by_dist[MAX_TRACKING_OBJECTS];

static int8_t (*Alarm_Level)(ufo_t *, ufo_t *);

/*
 * No any alarms issued by the firmware.
 * Rely upon high-level flight management software.
 */
static int8_t Alarm_None(ufo_t *this_aircraft, ufo_t *fop)
{
  return ALARM_LEVEL_NONE;
}

/*
 * Adjust relative altitude for relative vertical speed.
 */
float Adj_alt_diff(ufo_t *this_aircraft, ufo_t *fop)
{
  float alt_diff = fop->alt_diff;           /* positive means fop is higher than this_aircraft */
  float vsr = fop->vs - this_aircraft->vs;  /* positive means fop is rising relative to this_aircraft */
  if (abs(vsr) > 1000)  vsr = 0;            /* ignore implausible data */
  float alt_change = vsr * 0.05;  /* expected change in 10 seconds, converted to meters */

  /* only adjust towards higher alarm level: */
  if (alt_diff > 0 && alt_change < 0) {
    alt_diff += alt_change;   /* makes alt_diff smaller */
    if (alt_diff < 0)  return 0;  /* minimum abs_alt_diff */
  } else if (alt_diff < 0 && alt_change > 0) {
    alt_diff += alt_change;   /* makes alt_diff less negative */
    if (alt_diff > 0)  return 0;  /* minimum abs_alt_diff */
  }

  /* GPS altitude is fuzzy so ignore the first 60m difference */
  if (alt_diff > 0) {
    if (alt_diff < VERTICAL_SLACK)  return 0;
    return (alt_diff - VERTICAL_SLACK);
  }
  if (-alt_diff < VERTICAL_SLACK)  return 0;
  return (alt_diff + VERTICAL_SLACK);
}

/*
 * Simple, distance based alarm level assignment.
 */
static int8_t Alarm_Distance(ufo_t *this_aircraft, ufo_t *fop)
{
  int8_t rval = ALARM_LEVEL_NONE;

  if (this_aircraft->prevtime_ms == 0)
    return ALARM_LEVEL_NONE;

  int distance = (int) fop->distance;
  if (distance > 2.0*ALARM_ZONE_CLOSE
      || fabs(fop->alt_diff) > 2*VERTICAL_SEPARATION) {
    return ALARM_LEVEL_NONE;
    /* save CPU cycles */
  }
  
  int abs_alt_diff = abs((int) Adj_alt_diff(this_aircraft, fop));

  if (abs_alt_diff < VERTICAL_SEPARATION) {  /* no alarms if too high or too low */

    /* take altitude (and vert speed) differences into account */
    distance = VERTICAL_SLOPE * abs_alt_diff + distance;

    if (distance < ALARM_ZONE_URGENT) {
      rval = ALARM_LEVEL_URGENT;
    } else if (distance < ALARM_ZONE_IMPORTANT) {
      rval = ALARM_LEVEL_IMPORTANT;
    } else if (distance < ALARM_ZONE_LOW) {
      rval = ALARM_LEVEL_LOW;
    } else if (distance < ALARM_ZONE_CLOSE) {
      rval = ALARM_LEVEL_CLOSE;
    }
  }

  return rval;
}

/*
 * EXPERIMENTAL
 *
 * Linear, CoG and GS based collision prediction.
 */
static int8_t Alarm_Vector(ufo_t *this_aircraft, ufo_t *fop)
{
  int8_t rval = ALARM_LEVEL_NONE;

  if (this_aircraft->prevtime_ms == 0 || (fop->gnsstime_ms - fop->prevtime_ms > 3000))
    return ALARM_LEVEL_NONE;

  float distance = fop->distance;
  if (distance > 2*ALARM_ZONE_CLOSE
      || fabs(fop->alt_diff) > 2*VERTICAL_SEPARATION) {
    return ALARM_LEVEL_NONE;
    /* save CPU cycles */
  }

  if (distance / (fop->speed + this_aircraft->speed)
         > ALARM_TIME_CLOSE * _GPS_MPS_PER_KNOT) {
    return ALARM_LEVEL_NONE;
    /* save CPU cycles */
  }

  if (circling || fabs(this_aircraft->turnrate) > 3.0 || fabs(fop->turnrate) > 3.0)
    return Alarm_Distance(this_aircraft, fop);

  float abs_alt_diff = fabs(Adj_alt_diff(this_aircraft, fop));

  if (abs_alt_diff < VERTICAL_SEPARATION) {  /* no alarms if too high or too low */

    /* Subtract 2D velocity vector of traffic from 2D velocity vector of this aircraft */ 
    float V_rel_y = this_aircraft->speed * cos_approx(this_aircraft->course) -
                    fop->speed * cos_approx(fop->course);                      /* N-S */
    float V_rel_x = this_aircraft->speed * sin_approx(this_aircraft->course) -
                    fop->speed * sin_approx(fop->course);                      /* E-W */

    float V_rel_magnitude = sqrtf(V_rel_x * V_rel_x + V_rel_y * V_rel_y) * _GPS_MPS_PER_KNOT;
    float V_rel_direction = atan2_approx(V_rel_y, V_rel_x);     /* direction fop is coming from */

    /* +- some degrees tolerance for collision course */

    if (V_rel_magnitude > ALARM_VECTOR_SPEED) {

      /* time is seconds prior to impact */
      /* take altitude difference into account */

      float t = (distance + VERTICAL_SLOPE*abs_alt_diff) / V_rel_magnitude;

      float rel_angle = fabs(V_rel_direction - fop->bearing);

      if (rel_angle < ALARM_VECTOR_ANGLE) {

        /* time limit values are compliant with FLARM data port specs */

        if (t < ALARM_TIME_URGENT) {
          rval = ALARM_LEVEL_URGENT;
        } else if (t < ALARM_TIME_IMPORTANT) {
          rval = ALARM_LEVEL_IMPORTANT;
        } else if (t < ALARM_TIME_LOW) {
          rval = ALARM_LEVEL_LOW;
        } else if (t < ALARM_TIME_CLOSE) {
          rval = ALARM_LEVEL_CLOSE;
        }

      } else if (rel_angle < 2 * ALARM_VECTOR_ANGLE) {

        /* reduce alarm level since direction is less direct */

        if (t < ALARM_TIME_URGENT) {
          rval = ALARM_LEVEL_IMPORTANT;
        } else if (t < ALARM_TIME_IMPORTANT) {
          rval = ALARM_LEVEL_LOW;
        } else if (t < ALARM_TIME_LOW) {
          rval = ALARM_LEVEL_CLOSE;
/*      } else if (t < ALARM_TIME_CLOSE) {
          rval = ALARM_LEVEL_NONE;               */
        }

      } else if (rel_angle < 3 * ALARM_VECTOR_ANGLE) {

        /* further reduce alarm level for larger angles */

        if (t < ALARM_TIME_URGENT) {
          rval = ALARM_LEVEL_LOW;
        } else if (t < ALARM_TIME_IMPORTANT) {
          rval = ALARM_LEVEL_CLOSE;
/*      } else if (t < ALARM_TIME_LOW) {
          rval = ALARM_LEVEL_NONE;               */
        }
      }
    }
  }
  return rval;
}


/*
 * VERY EXPERIMENTAL
 *
 * "Legacy" method is based on short history of (future?) 2D velocity vectors (NS/EW).
 * The approach here tries to use the velocity components from 4 time points
 * that FLARM sends out, in the way it probably intended by sending the data
 * out in that format, and given the weak computing power of early hardware.
 */
static int8_t Alarm_Legacy(ufo_t *this_aircraft, ufo_t *fop)
{
  int8_t rval = ALARM_LEVEL_NONE;

  /* TBD */

  return rval;
}

void Traffic_Update(ufo_t *fop)
{
  /* use an approximation for distance & bearing between 2 points */
  float x, y;
  y = fop->latitude - ThisAircraft.latitude;         /* degrees */
  x = (fop->longitude - ThisAircraft.longitude) * CosLat(ThisAircraft.latitude);
  fop->distance = 111300.0 * sqrtf(x*x + y*y);       /* meters  */
  fop->bearing = atan2_approx(y, x);           /* degrees from ThisAircraft to fop */

  fop->alt_diff = fop->altitude - ThisAircraft.altitude;

  if (Alarm_Level) {

    fop->alarm_level = (*Alarm_Level)(&ThisAircraft, fop);

    /* if gone farther, reduce threshold for a new alert - with hysteresis. */
    /* E.g., if alarm was for LOW, alert_level was set to IMPORTANT.        */
    /* A new alarm alert will sound if close enough to now be URGENT.       */
    /* Or, if now gone to CLOSE (farther than LOW), set alert_level to LOW, */
    /* then next time reaches alarm_level IMPORTANT will give a new alert.  */
    /* Or, if now gone to NONE (farther than CLOSE), set alert_level to     */
    /* CLOSE, then next time returns to alarm_level LOW will give an alert. */

    if (fop->alarm_level < fop->alert_level)       /* if just less by 1...   */
         fop->alert_level = fop->alarm_level + 1;  /* ...then no change here */
  }
}

void ParseData()
{
    size_t rx_size = RF_Payload_Size(settings->rf_protocol);
    rx_size = rx_size > sizeof(fo.raw) ? sizeof(fo.raw) : rx_size;

#if DEBUG
    Hex2Bin(TxDataTemplate, RxBuffer);
#endif

    memset(fo.raw, 0, sizeof(fo.raw));
    memcpy(fo.raw, RxBuffer, rx_size);

    if (settings->nmea_p) {
      StdOut.print(F("$PSRFI,"));
      StdOut.print((unsigned long) now()); StdOut.print(F(","));
      StdOut.print(Bin2Hex(fo.raw, rx_size)); StdOut.print(F(","));
      StdOut.println(RF_last_rssi);
    }

    if (memcmp(RxBuffer, TxBuffer, rx_size) == 0) {
      if (settings->nmea_p) {
        StdOut.println(F("$PSRFE,RF loopback is detected"));
      }
      return;
    }

    fo = EmptyFO;  /* to ensure no data from past packets remains in any field */

    if (protocol_decode && (*protocol_decode)((void *) RxBuffer, &ThisAircraft, &fo)) {

      if (fo.addr == settings->ignore_id) {        /* ID told in settings to ignore */
             return;
      } else if (fo.addr == ThisAircraft.addr) {
             /* received same ID as this aircraft, and not told to ignore it */
             /* then replace ID with a random one */
             settings->id_method = ADDR_TYPE_ANONYMOUS;
             generate_random_id();
             return;
      }

      fo.rssi = RF_last_rssi;

      Traffic_Update(&fo);

      int i;

      /* first check whether we are already tracking this object */
      /* overwrite old data, but preserve fields that store history */
      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
        if (Container[i].addr == fo.addr) {
          uint8_t alert_bak = Container[i].alert;
          uint8_t level_bak = Container[i].alert_level;
          float prevcourse = Container[i].course;
          uint32_t prevtime_ms = Container[i].gnsstime_ms;
          Container[i] = fo;   /* copies the whole object/structure */
          Container[i].prevcourse = prevcourse;
          Container[i].prevtime_ms = prevtime_ms;
          Container[i].alert = alert_bak;
          Container[i].alert_level = level_bak;
          return;
        }
      }

      /* new object, try and find a slot for it */

      /* replace an empty or expired object if found */
      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
/*      if (Container[i].addr == 0) {   // then .timestamp is also 0
          Container[i] = fo;
          return;
        }
*/      if (now() - Container[i].timestamp > ENTRY_EXPIRATION_TIME) {
          Container[i] = fo;
          return;
        }
      }

      /* may need to replace a non-expired object:   */
      /* identify the least important current object */

#if !defined(EXCLUDE_TRAFFIC_FILTER_EXTENSION)

      /* replace an object of lower alarm level if found */
      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
        if (fo.alarm_level > Container[i].alarm_level) {
          Container[i] = fo;
          return;
        }
      }

      /* identify the farthest-away object */
      /* (distance adjusted for altitude difference) */

      int max_dist_ndx = 0;
      float adj_max_dist = 0;
      float adj_distance = 0;

      for (i=0; i < MAX_TRACKING_OBJECTS; i++) {
        adj_distance = Container[i].distance
               + VERTICAL_SLOPE * fabs(Adj_alt_diff(&ThisAircraft,&Container[i]));
        if (adj_distance > adj_max_dist)  {
          max_dist_ndx = i;
          adj_max_dist = adj_distance;
        }
      }

      /* replace the farthest currently-tracked object, but only if  */
      /* the new object is closer, and of same or higher alarm level */
      if ((fo.distance + VERTICAL_SLOPE*fabs(Adj_alt_diff(&ThisAircraft,&fo))
               < adj_max_dist)
        &&
          fo.alarm_level >= Container[max_dist_ndx].alarm_level) {
        Container[max_dist_ndx] = fo;
        return;
      }
#endif /* EXCLUDE_TRAFFIC_FILTER_EXTENSION */

       /* otherwise, no slot found, ignore the new object */
    }
}

void Traffic_setup()
{
  switch (settings->alarm)
  {
  case TRAFFIC_ALARM_NONE:
    Alarm_Level = &Alarm_None;
    break;
  case TRAFFIC_ALARM_VECTOR:
    Alarm_Level = &Alarm_Vector;
    break;
  case TRAFFIC_ALARM_LEGACY:
    Alarm_Level = &Alarm_Legacy;
    break;
  case TRAFFIC_ALARM_DISTANCE:
  default:
    Alarm_Level = &Alarm_Distance;
    break;
  }
}

void Traffic_loop()
{
  if (isTimeToUpdateTraffic()) {

    ufo_t *mfop = NULL;
    int max_alarm_level = ALARM_LEVEL_NONE;
        
    for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {

      ufo_t *fop = &Container[i];

      if (fop->addr) {  /* non-empty ufo */
      
        if (ThisAircraft.timestamp - fop->timestamp <= ENTRY_EXPIRATION_TIME) {

          if ((ThisAircraft.timestamp - fop->timestamp) >= TRAFFIC_VECTOR_UPDATE_INTERVAL)
              Traffic_Update(fop);

          /* figure out what is the highest alarm level needing a sound alert */
          if (fop->alarm_level > fop->alert_level
                  && fop->alarm_level > ALARM_LEVEL_CLOSE) {
              if (fop->alarm_level > max_alarm_level) {
                  max_alarm_level = fop->alarm_level;
                  mfop = fop;
              }
          }

        } else {   /* expired ufo */

          *fop = EmptyFO;
          /* implied by emptyFO:
          fop->addr = 0;
          fop->alert = 0;
          fop->alarm_level = 0;
          fop->alert_level = 0;
          fop->prevtime_ms = 0;
          etc... */
        }
      }
    }

    /* sound an alarm if new alert, or got two levels closer than previous  */
    /* alert, or hysteresis: got two levels farther, and then closer.       */
    /* E.g., if alarm was for LOW, alert_level was set to IMPORTANT.        */
    /* A new alarm alert will sound if close enough to now be URGENT.       */
    /* Or, if now gone to CLOSE (farther than LOW), set alert_level to LOW, */
    /* then next time reaches alarm_level IMPORTANT will give a new alert.  */
    /* Or, if now gone to NONE (farther than CLOSE), set alert_level to     */
    /* CLOSE, then next time returns to alarm_level LOW will give an alert. */

    if (max_alarm_level > ALARM_LEVEL_CLOSE) {
      Sound_Notify(max_alarm_level);
      if (mfop != NULL) {
        mfop->alert_level = mfop->alarm_level + 1;
        mfop->alert |= TRAFFIC_ALERT_SOUND;  /* no longer actually used */
      }
    }

    UpdateTrafficTimeMarker = millis();
  }
}

void ClearExpired()
{
  for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].addr &&
         (ThisAircraft.timestamp - Container[i].timestamp) > ENTRY_EXPIRATION_TIME) {
      Container[i] = EmptyFO;
    }
  }
}

int Traffic_Count()
{
  int count = 0;

  for (int i=0; i < MAX_TRACKING_OBJECTS; i++) {
    if (Container[i].addr) {
      count++;
    }
  }

  return count;
}

/* this is used in Text_EPD.cpp for 'radar' display, */
/* so don't adjust for altitude difference.          */

int traffic_cmp_by_distance(const void *a, const void *b)
{
  traffic_by_dist_t *ta = (traffic_by_dist_t *)a;
  traffic_by_dist_t *tb = (traffic_by_dist_t *)b;

  if (ta->distance >  tb->distance) return  1;
/*  if (ta->distance == tb->distance) return  0; */
  if (ta->distance <  tb->distance) return -1;
  return  0;
}

/* called (as needed) from softRF.ino normal(), */
/*   or from ParseData() above,                 */
/*   or every few minutes from Estimate_Wind()  */
void generate_random_id()
{
    uint32_t id = millis();
    id = (id ^ (id<<5) ^ (id>>5)) & 0x000FFFFF;
    if (settings->id_method == ADDR_TYPE_RANDOM)
      id |= 0x00E00000;
    else
      id |= 0x00F00000;
    ThisAircraft.addr = id;
}
