#include "acmap.hpp"
#include "wlserver.hpp"
#include <stdio.h>

std::vector<actionmapping> g_ActionMap;
bool g_bReportActionMaps;

static int findmap(uint32_t srcdev, uint32_t srcid)
{
	if ( g_ActionMap.empty() == false )
	{
		for ( actionmapping& am : g_ActionMap )
		{
			if ( am.src.dev == srcdev && am.src.id == srcid )
			{
				return &am - &g_ActionMap[0];
			}
		}
	}
	return -1;
}

void actionmap_reset()
{
	g_ActionMap = std::vector<actionmapping>();
}

void actionmap_set(uint32_t srcdev, uint32_t srcid, uint32_t dstdev, uint32_t dstid)
{
	int index = findmap(srcdev, srcid);
	if ( index == -1 )
	{
		g_ActionMap.push_back({{srcdev, srcid}, {dstdev, dstid}});
	}
	else
	{
		g_ActionMap[index].dst.dev = dstdev;
		g_ActionMap[index].dst.id = dstid;
	}
}

void actionmap_get(uint32_t srcdev, uint32_t srcid, uint32_t* dstdev, uint32_t* dstid)
{
	int index = findmap(srcdev, srcid);
	if (index == -1 )
	{
		*dstdev = srcdev;
		*dstid = srcid;
	}
	else
	{
		*dstdev = g_ActionMap[index].dst.dev;
		*dstid = g_ActionMap[index].dst.id;
	}
}

void actionmap_unset(uint32_t dev, uint32_t id)
{
	int index = findmap(dev, id);
	if (index != -1)
	{
		g_ActionMap.erase(g_ActionMap.begin() + index);
	}
}

static int chtodev(char ch)
{
	return ch == ACDEV_KEYBOARD || ch == ACDEV_BUTTON || ch == ACDEV_NULL ? ch : ACDEV_NONE;
}

void actionmap_load(const char* filename)
{
	FILE* f = fopen(filename, "r");
	if (!f)
	{
		fprintf(stderr, "gamescope: actionmap file '%s' not found\n", filename);
		return;
	}
	while (!feof(f))
	{
		char srcdev = 0, dstdev = 0;
		int srcid, dstid;
		fscanf(f, "%c %i %c %i\n", &srcdev, &srcid, &dstdev, &dstid);
		srcdev = chtodev(srcdev);
		dstdev = chtodev(dstdev);
		if (srcdev && dstdev)
		{
			actionmap_set(srcdev, srcid, dstdev, dstid);
		}
	}
	fclose(f);
}

void actionmap_perform(uint32_t dev, uint32_t id, int value, uint32_t time)
{
	uint32_t dstdev, dstid;
	actionmap_get(dev, id, &dstdev, &dstid);
	if (g_bReportActionMaps)
	{
		fprintf(stderr, "actionmap: %c %i -> %c %i\n", dev, id, dstdev, dstid);
	}
	switch (dstdev)
	{
	case ACDEV_KEYBOARD:
		wlserver_key(dstid, value != 0, time);
		break;
	case ACDEV_BUTTON:
		wlserver_mousebutton(dstid, value != 0, time);
		break;
	}
}

void actionmap_performkey(uint32_t key, int down, uint32_t time)
{
	actionmap_perform(ACDEV_KEYBOARD, key, down ? 1 : 0, time);
}

void actionmap_performbutton(uint32_t button, int down, uint32_t time)
{
	actionmap_perform(ACDEV_BUTTON, button, down ? 1 : 0, time);
}
