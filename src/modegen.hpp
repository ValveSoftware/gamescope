#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <xf86drmMode.h>

void generate_cvt_mode(drmModeModeInfo *mode, int hdisplay, int vdisplay,
	float vrefresh, bool reduced, bool interlaced);
void generate_fixed_mode(drmModeModeInfo *mode, const drmModeModeInfo *base,
	float vrefresh);
