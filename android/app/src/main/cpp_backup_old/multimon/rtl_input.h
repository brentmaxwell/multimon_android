/*
 *      rtl_input.h -- RTL-SDR input support for multimon-ng
 *
 *      Copyright (C) 2026
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 */

#ifndef _RTL_INPUT_H
#define _RTL_INPUT_H

#include <stdint.h>

int rtl_init(const char *device, uint32_t frequency, uint32_t sample_rate, int gain, int ppm_error,
             void (*process_callback)(int16_t *buffer, int length));

int rtl_start(void);

void rtl_stop(void);

#endif /* _RTL_INPUT_H */
