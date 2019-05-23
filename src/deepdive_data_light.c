/* 
  Unofficial driver for Vive Trackers and up to two lighthouses, with an
    emphasis on pulling tracker and lighthouse calibration data from devices.
  
  Adapted from: https://github.com/cnlohr/libsurvive
  Which was based off: https://github.com/collabora/OSVR-Vive-Libre
    Originally Copyright 2016 Philipp Zabel
    Originally Copyright 2016 Lubosz Sarnecki <lubosz.sarnecki@collabora.co.uk>
    Originally Copyright (C) 2013 Fredrik Hultin
    Originally Copyright (C) 2013 Jakob Bornecrantz
  Using documentation from: https://github.com/nairol/LighthouseRedox
  
  Copyright (c) 2017 Andrew Symington

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "deepdive_data_light.h"

#include <zlib.h>

// OOTX CODE

// Avoids compiler reording with -O2
union custom_float {
  uint32_t i;
  float f;
};

// Converts a 16bit float to a 32 bit float
static float convert_float(uint8_t* data) {
  uint16_t x = *(uint16_t*)data;
  union custom_float fnum;
  fnum.f = 0;
  // handle sign
  fnum.i = (x & 0x8000)<<16;
  if ((x & 0x7FFF) == 0) return fnum.f; //signed zero
  if ((x & 0x7c00) == 0) {
    // denormalized
    x = (x&0x3ff)<<1; // only mantissa, advance intrinsic bit forward
    uint8_t e = 0;
    // shift until intrinsic bit of mantissa overflows into exponent
    // increment exponent each time
    while ((x&0x0400) == 0) {
      x<<=1;
      e++;
    }
    fnum.i |= ((uint32_t)(112-e))<<23;    // bias exponent to 127
    fnum.i |= ((uint32_t)(x&0x3ff))<<13;  // insert mantissa
    return fnum.f;
  }
  if((x&0x7c00) == 0x7c00) {
    // for infinity, fraction is 0; for NaN, fraction is anything non zero
    // we shift because the mantissa of a NaN can have meaning
    fnum.i |= 0x7f800000 | ((uint32_t)(x & 0x3ff))<<13;
    return fnum.f;
  }
  fnum.i |= ((((uint32_t)(x & 0x7fff)) + 0x1c000u) << 13);
  return fnum.f;
}

// Convert the packet
static void decode_packet(struct Tracker *tracker, uint8_t id,
  uint8_t *data, uint32_t tc) {
  // Pop the serial number off the packet, so we can perform a lookup
  static char serial[MAX_SERIAL_LENGTH];
  sprintf(serial, "%u", *(uint32_t*)(data + 0x02));

  // We need to search to see if we already know about this LH
  uint8_t idx, available = MAX_NUM_LIGHTHOUSES;
  for (idx = 0; idx < MAX_NUM_LIGHTHOUSES; idx++) {
    // If there is not timestamp with this lighthouse, then this is a
    // free record, which we will use in the case the serial is not found.
    if (!tracker->driver->lighthouses[idx].timestamp &&
      available == MAX_NUM_LIGHTHOUSES) available = idx;
    // If we find the tracker
    if (!strcmp(tracker->driver->lighthouses[idx].serial, serial))
      break;
  }

  // We should never really be in the position where we get OOTX packets
  // from more than two lighthouses. But, if we do, you should see this...
  if (idx >= MAX_NUM_LIGHTHOUSES) {
    if (available == MAX_NUM_LIGHTHOUSES) {
      printf("We appear to have seen more than MAX_NUM_LIGHTHOUSES\n");
      printf("We are therefore going to disregard this OOTX data :(\n");
      return;
    }
    idx = available;
  }

  // Populate this data
  struct Lighthouse *lh = &tracker->driver->lighthouses[idx]; 
  strcpy(lh->serial, serial);
  lh->fw_version = *(uint16_t*)(data + 0x00);
  lh->motors[0].phase = convert_float(data + 0x06);
  lh->motors[1].phase = convert_float(data + 0x08);
  lh->motors[0].tilt = convert_float(data + 0x0a);
  lh->motors[1].tilt = convert_float(data + 0x0c);
  lh->sys_unlock_count = *(uint8_t*)(data + 0x0e);
  lh->hw_version = *(uint8_t*)(data + 0x0f);
  lh->motors[0].curve = convert_float(data + 0x10);
  lh->motors[1].curve = convert_float(data + 0x12);
  lh->accel[0] = *(int8_t*)(data + 0x14);
  lh->accel[1] = *(int8_t*)(data + 0x15);
  lh->accel[2] = *(int8_t*)(data + 0x16);
  lh->motors[0].gibphase = convert_float(data + 0x17);
  lh->motors[1].gibphase = convert_float(data + 0x19);
  lh->motors[0].gibmag = convert_float(data + 0x1b);
  lh->motors[1].gibmag = convert_float(data + 0x1d);
  lh->mode_current = *(int8_t*)(data + 0x1f);
  lh->sys_faults = *(int8_t*)(data + 0x20);
  lh->timestamp = tc;
  // printf("[%10u] Tracker # %s rx config for LH %s (id: %u)\n",
  //  tc, tracker->serial, lh->serial, id);

  // There is no guarantee that two given trackers will enumerate the
  // same lighthouses as id 0 and id 1. So we need a lookup!
  tracker->ootx[id].lighthouse = lh;

  // Push the new lighthouse data to the callee
  if (tracker->driver->lighthouse_fn)
    tracker->driver->lighthouse_fn(lh);
}

// Swap endianness of 16 bit unsigned integer
static uint16_t swaps(uint16_t val) {
    return    ((val << 8) & 0xff00) 
            | ((val >> 8) & 0x00ff);
}

// Swap endianness of 32 bit unsigned integer
static uint32_t swapl(uint32_t val) {
  return      ((val >> 24) & 0x000000ff)
            | ((val << 8)  & 0x00ff0000)
            | ((val >> 8)  & 0x0000ff00)
            | ((val << 24) & 0xff000000);
}

// Process a single bit of the OOTX data
static void ootx_feed(struct Tracker *tracker, 
  uint8_t lh, uint8_t bit, uint32_t tc) {
  // OOTX decoders to gather base station configuration
  if (lh >= MAX_NUM_LIGHTHOUSES)
    return;
  // Get the correct context for this OOTX
  OOTX *ctx = &tracker->ootx[lh];
  // Always check for preamble and reset if needed
  if (bit) {
    if (ctx->preamble >= PREAMBLE_LENGTH) {
      // printf("Preamble found\n");
      ctx->state = LENGTH;
      ctx->length = 0;
      ctx->pos = 0;
      ctx->syn = 0;
      ctx->preamble = 0;
      return;
    }
    ctx->preamble = 0;
  } else {
    ctx->preamble++;
  }
  
  // State machine
  switch (ctx->state) {
  case PREAMBLE:
    return;
  case LENGTH:
    if (ctx->syn == 16) {
      ctx->length = swaps(ctx->length);   // LE to BE
      // printf("LEN: %u for LH %u\n", ctx->length, lh);
      ctx->pad = (ctx->length % 2);       // Force even num bytes
      // printf("PAD: %u for LH %u\n", ctx->pad , lh);
      ctx->state = PREAMBLE;
      if (ctx->length + ctx->pad <= MAX_PACKET_LEN) {
        // printf("[PRE] -> [PAY]\n");
        ctx->state = PAYLOAD;
        ctx->syn = ctx->pos = 0;
        memset(ctx->data, 0x0, MAX_PACKET_LEN);
      }
      return;
    }
    ctx->length |= (((uint16_t)bit) << (15 - ctx->syn++));
    return;
  case PAYLOAD:
    // Decrement the pointer every 8 bits to find the byte offset
    if (ctx->syn == 8 || ctx->syn == 16) {
      // Increment the byte offset
      ctx->pos++;
      // If we can't decrement pointer then we have received all the data
      if (ctx->pos == ctx->length + ctx->pad) {
        // printf("[PAY] -> [CRC]\n");
        ctx->state = CHECKSUM;
        ctx->syn = ctx->pos = 0;
        ctx->crc = 0;
        return;
      }
    }
    // The 17th bit is a sync bit, and should be swallowed
    if (ctx->syn == 16) {
      ctx->syn = 0;
      return;
    }
    // Append data to the current byte in the sequence
    ctx->data[ctx->pos] |= (bit << (7 - ctx->syn++ % 8));
    return;
  case CHECKSUM:
    // Decrement the pointer every 8 bits to find the byte offset
    if (ctx->syn == 8 || ctx->syn == 16) {
      ctx->pos++;
      if (ctx->pos == 4) {
        // Calculate the CRC
        uint32_t crc = crc32( 0L, 0 /*Z_NULL*/, 0 );
        crc = crc32(crc, ctx->data, ctx->length);
        // Print some debug info
        // printf("[CRC] -> [PRE]\n");
        // printf("[CRC] RX = %08x\n", swapl(ctx->crc));
        // printf("[CRC] CA = %08x\n", crc);
        if (crc == swapl(ctx->crc))
          decode_packet(tracker, lh, ctx->data, tc);
        // Return to state
        ctx->state = PREAMBLE;
        ctx->pos = ctx->syn = 0;
        ctx->preamble = 0;
        ctx->length = 0;
        return;
      }
    }
    // The 17th bit is a sync bit, and should be swallowed
    if (ctx->syn == 16) {
      ctx->syn = 0;
      return;
    }
    ctx->crc |= (((uint32_t)bit) << (31 - (ctx->pos * 8 + ctx->syn++ % 8)));
    return;
  }
}

// LIGHTCAP

// Get the acode from the 
int handle_acode(lightcap_data* lcd, int length) {
  double old_offset = lcd->global.acode_offset;
  double new_offset = (((length) + 250) % 500) - 250;
  lcd->global.acode_offset = old_offset * 0.9 + new_offset * 0.1;
  return (uint8_t)((length - 2750) / 500);
}

// Handle measuements
void handle_measurements(struct Tracker * tracker) {
  // Get the tracker-specific lightcap data
  lightcap_data* lcd = &tracker->lcd;
  unsigned int longest_pulse = 0;
  unsigned int timestamp_of_longest_pulse = 0;
  for (int i = 0; i < MAX_NUM_SENSORS; i++) {
    if (lcd->sweep.sweep_len[i] > longest_pulse) {
      longest_pulse = lcd->sweep.sweep_len[i];
      timestamp_of_longest_pulse = lcd->sweep.sweep_time[i];
    }
  }
  int allZero = 1;
  for (int q = 0; q < MAX_NUM_SENSORS; q++)
    if (lcd->sweep.sweep_len[q] != 0)
      allZero = 0;

  // Temporary data structures to hold results
  static uint16_t num_sensors;
  static uint16_t sensors[MAX_NUM_SENSORS];
  static uint32_t sweeptimes[MAX_NUM_SENSORS];
  static uint32_t angles[MAX_NUM_SENSORS];
  static uint16_t lengths[MAX_NUM_SENSORS];
  static uint32_t st;
  static uint8_t lh;
  static uint8_t ax;

  // Get the sync pulse rising edge time, lighthouse and axis
  st = lcd->per_sweep.activeSweepStartTime;
  lh = lcd->per_sweep.activeLighthouse;
  ax = lcd->per_sweep.activeAcode & 1;

  // Get the rotation based on the axis and negate Y to 
  uint8_t motor = (ax == 0 ? MOTOR_AXIS0 : MOTOR_AXIS1);

  // Copy over the final data
  num_sensors = 0;
  for (int i = 0; i < MAX_NUM_SENSORS; i++) {
    static int counts[MAX_NUM_SENSORS][2] = {0};
    if (lcd->per_sweep.activeLighthouse > -1 && !allZero)
      if (lcd->sweep.sweep_len[i] == 0)
        counts[i][lcd->per_sweep.activeAcode & 1] = 0;
    if (lcd->sweep.sweep_len[i] != 0) {
      sensors[num_sensors] = i;
      sweeptimes[num_sensors] = lcd->sweep.sweep_time[i];
      angles[num_sensors] = lcd->sweep.sweep_time[i]
        - lcd->per_sweep.activeSweepStartTime + lcd->sweep.sweep_len[i] / 2;
      lengths[num_sensors] = lcd->sweep.sweep_len[i];
      num_sensors++;
    }
  }

  // Push off the measurement bundle ONLY when we have received
  // an OOTX from the current lighthouse and if we have data
  if (num_sensors > 0 && tracker->ootx[lh].lighthouse) {
    if (tracker->driver->lig_fn)
      tracker->driver->lig_fn(tracker, tracker->ootx[lh].lighthouse,
        motor, st, num_sensors, sensors, sweeptimes, angles, lengths);
  }

  // Clear memory
  memset(&lcd->sweep, 0, sizeof(lightcaps_sweep_data));
}

// Handle sync
void handle_sync(struct Tracker * tracker,
  uint32_t timecode, uint16_t sensor, uint16_t length) {
  // Get the tracker-specific lightcap data
  lightcap_data* lcd = &tracker->lcd;
  // Get the acode from the sendor treading
  int acode = handle_acode(lcd, length);
  // Process any cached measurements
  handle_measurements(tracker);
  // Calculate the time since last sweet
  int time_since_last_sync = (timecode - lcd->per_sweep.recent_sync_time);
  // Store up sync pulses so we can take the earliest starting time 
  if (time_since_last_sync < 2400) {
    lcd->per_sweep.recent_sync_time = timecode;
    if (length > lcd->per_sweep.lh_max_pulse_length[lcd->per_sweep.current_lh]) {
      lcd->per_sweep.lh_max_pulse_length[lcd->per_sweep.current_lh] = length;
      lcd->per_sweep.lh_start_time[lcd->per_sweep.current_lh] = timecode + length;
      lcd->per_sweep.lh_acode[lcd->per_sweep.current_lh] = acode;
    }
  } else if (time_since_last_sync < 24000) {
    lcd->per_sweep.activeLighthouse = -1;
    lcd->per_sweep.recent_sync_time = timecode;
    lcd->per_sweep.current_lh = 1;
    lcd->per_sweep.lh_start_time[lcd->per_sweep.current_lh] = timecode;
    lcd->per_sweep.lh_max_pulse_length[lcd->per_sweep.current_lh] = length + length;
    lcd->per_sweep.lh_acode[lcd->per_sweep.current_lh] = acode;
  } else if (time_since_last_sync > 370000) {
    // Initialize here
    memset(&lcd->per_sweep, 0, sizeof(lcd->per_sweep));
    lcd->per_sweep.activeLighthouse = -1; 
    for (uint8_t i = 0; i < MAX_NUM_LIGHTHOUSES; ++i)
      lcd->per_sweep.lh_acode[i] = -1;
    lcd->per_sweep.recent_sync_time = timecode;
    lcd->per_sweep.current_lh = 0;
    lcd->per_sweep.lh_start_time[lcd->per_sweep.current_lh] = timecode + length;
    lcd->per_sweep.lh_max_pulse_length[lcd->per_sweep.current_lh] = length;
    lcd->per_sweep.lh_acode[lcd->per_sweep.current_lh] = acode;
  }
  // Feed the lighthouse OOTX decoder with the data bit
  if (lcd->per_sweep.current_lh < MAX_NUM_LIGHTHOUSES) {
    ootx_feed(tracker, lcd->per_sweep.current_lh,
     ((acode & 0x2) ? 1 : 0), timecode);
  }
}

// Called to process sweep events
void handle_sweep(struct Tracker * tracker,
  uint32_t timecode, uint16_t sensor, uint16_t length) {
  // Get the tracker-specific lightcap data
  lightcap_data* lcd = &tracker->lcd;
  // Reset the active lighthouse, start time and acode
  lcd->per_sweep.activeLighthouse = -1;
  lcd->per_sweep.activeSweepStartTime = 0;
  lcd->per_sweep.activeAcode = 0;
  for (uint8_t i = 0; i < MAX_NUM_LIGHTHOUSES; ++i) {
    int acode = lcd->per_sweep.lh_acode[i];
    if ((acode >= 0) && !(acode >> 2 & 1)) {
      lcd->per_sweep.activeLighthouse = i;
      lcd->per_sweep.activeSweepStartTime = lcd->per_sweep.lh_start_time[i];
      lcd->per_sweep.activeAcode = acode;
    }
  }
  // Check that we have an active lighthouse
  if (lcd->per_sweep.activeLighthouse < 0)
    return;
  // Store the data
  if (lcd->sweep.sweep_len[sensor] < length) {
    lcd->sweep.sweep_len[sensor] = length;
    lcd->sweep.sweep_time[sensor] = timecode;
  }
}

void deepdive_data_light(struct Tracker * tracker,
  uint32_t timecode, uint16_t sensor, uint16_t length) {
  if (sensor > MAX_NUM_SENSORS) return;
  if (length > 6750) return;
  if (length > 2750)
    handle_sync(tracker, timecode, sensor, length);
  else
    handle_sweep(tracker, timecode, sensor, length);
}
