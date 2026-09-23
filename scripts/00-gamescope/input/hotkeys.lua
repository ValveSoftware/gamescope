gamescope.config.input.hotkeys = {
    {
        -- XKB_KEY_Super_L + XKB_KEY_f
        keys = { 65515, 102 },
        func = function()
            command({ "fullscreen" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_n
        keys = { 65515, 110 },
        func = function()
            command({ "upscale_filter", "nearest" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_b
        keys = { 65515, 98 },
        func = function()
            command({ "upscale_filter", "linear" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_u
        keys = { 65515, 117 },
        func = function()
            command({ "upscale_filter", "fsr" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_y
        keys = { 65515, 121 },
        func = function()
            command({ "upscale_filter", "nis" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_i
        keys = { 65515, 105 },
        func = function()
            command({ "upscale_filter_sharpness", "up" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_o
        keys = { 65515, 111 },
        func = function()
            command({ "upscale_filter_sharpness", "down" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_s
        keys = { 65515, 115 },
        func = function()
            command({ "screenshot" })
        end
    },
    {
        -- XKB_KEY_Super_L + XKB_KEY_g
        keys = { 65515, 103 },
        func = function()
            command({ "keyboard_grab" })
        end
    },
}

debug("Registered hotkeys")