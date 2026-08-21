#pragma once

#include <xkbcommon/xkbcommon.h>

namespace gamescope
{
    uint32_t XkbKeymapModMask( xkb_keymap *keymap, const char *name );
}
