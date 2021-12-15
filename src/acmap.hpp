// Action maps

#pragma once

#include <stdint.h>
#include <vector>

#define ACDEV_NONE	0
#define ACDEV_KEYBOARD	'k'
#define ACDEV_BUTTON	'b'

struct action {
	uint32_t dev, id;
};

struct actionmapping {
	action src, dst;
};

extern std::vector<actionmapping> g_ActionMap;
extern bool g_bReportActionMaps;

void actionmap_reset();
void actionmap_set(uint32_t srcdev, uint32_t srcid, uint32_t dstdev, uint32_t dstid);
void actionmap_get(uint32_t srcdev, uint32_t srcid, uint32_t* dstdev, uint32_t* dstid);
void actionmap_unset(uint32_t dev, uint32_t id);
void actionmap_load(const char* filename);
void actionmap_perform(uint32_t dev, uint32_t id, int value, uint32_t time);
void actionmap_performkey(uint32_t key, int down, uint32_t time);
void actionmap_performbutton(uint32_t button, int down, uint32_t time);
