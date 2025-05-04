gamescope.config.input.hotkeys = {
    {
        -- XKB_KEY_Super_L + XKB_KEY_f
        keys = { 65515, 102 },
        command = { "fullscreen" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_n
        keys = { 65515, 110 },
        command = { "upscale_filter", "nearest" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_b
        keys = { 65515, 98 },
        command = { "upscale_filter", "linear" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_u
        keys = { 65515, 117 },
        command = { "upscale_filter", "fsr" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_y
        keys = { 65515, 121 },
        command = { "upscale_filter", "nis" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_i
        keys = { 65515, 105 },
        command = { "upscale_filter_sharpness", "up" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_o
        keys = { 65515, 111 },
        command = { "upscale_filter_sharpness", "down" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_s
        keys = { 65515, 115 },
        command = { "screenshot" }
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_g
        keys = { 65515, 103 },
        command = { "keyboard_grab" }
    },
}

debug("Registered hotkeys")