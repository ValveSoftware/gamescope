#include "Xkb.h"

namespace gamescope
{
    uint32_t XkbKeymapModMask( xkb_keymap *keymap, const char *name )
    {
        if ( !keymap )
            return 0u;

        xkb_mod_index_t index = xkb_keymap_mod_get_index( keymap, name );
        if ( index == XKB_MOD_INVALID || index >= 32)
            return 0u;

        return 1u << index;
    }
}
