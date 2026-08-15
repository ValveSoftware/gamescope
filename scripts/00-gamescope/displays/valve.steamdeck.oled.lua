local steamdeck_oled_hdr = {
    supported = true,
    force_enabled = true,
    eotf = gamescope.eotf.gamma22,
    max_content_light_level = 1000,
    max_frame_average_luminance = 800,
    min_content_light_level = 0
}

local steamdeck_oled_refresh_rates = {
    45, 47, 48, 49,
    50, 51, 53, 55, 56, 59,
    60, 62, 64, 65, 66, 68,
    72, 73, 76, 77, 78,
    80, 81, 82, 84, 85, 86, 87, 88,
    90
}

gamescope.config.known_displays.steamdeck_oled_sdc = {
    pretty_name = "Steam Deck OLED (SDC)",
    hdr = steamdeck_oled_hdr,
    dynamic_refresh_rates = steamdeck_oled_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for Steam Deck OLED (SDC)")
        local vfps = {
            1321, 1264, 1209, 1157, 1106,
            1058, 993,  967,  925,  883,  829,  805,  768,  732,  698,
            665,  632,  601,  571,  542,  501,  486,  459,  433,  408,
            383,  360,  337,  314,  292,  271,  250,  230,  210,  191,
            173,  154,  137,  119,  102,  86,   70,   54,   38,   23,
            9
        }
        local vfp = vfps[zero_index(refresh - 45)]
        if vfp == nil then
            warn("Couldn't do refresh "..refresh.." on Steam Deck OLED (SDC)")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    matches = function(display)
        if display.vendor == "VLV" and display.product == 0x3003 then
            debug("[steamdeck_oled_sdc] Matched VLV and product 0x3003")
            -- Higher priority than LCD.
            return 5100
        end

        return -1
    end
}
debug("Registered Steam Deck OLED (SDC) as a known display")
--debug(inspect(gamescope.config.known_displays.steamdeck_oled_sdc))

gamescope.config.known_displays.steamdeck_oled_boe = {
    pretty_name = "Steam Deck OLED (BOE)",
    hdr = steamdeck_oled_hdr,
    dynamic_refresh_rates = steamdeck_oled_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode for "..refresh.." for Steam Deck OLED (BOE)")

        local vfps = {
            1320, 1272, 1216, 1156, 1112,
            1064, 992,  972,  928,  888,  828,  808,  772,  736,  700,
            664,  636,  604,  572,  544,  500,  488,  460,  436,  408,
            384,  360,  336,  316,  292,  272,  252,  228,  212,  192,
            172,  152,  136,  120,  100,  84,   68,   52,   36,   20,
            8
        }
        local vfp = vfps[zero_index(refresh - 45)]
        if vfp == nil then
            warn("Couldn't do refresh "..refresh.." on Steam Deck OLED (BOE)")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    matches = function(display)
        if display.vendor == "VLV" and display.product == 0x3004 then
            debug("[steamdeck_oled_boe] Matched VLV and product 0x3004")
            -- Higher priority than LCD.
            return 5100
        end

        return -1
    end
}
debug("Registered Steam Deck OLED (BOE) as a known display")
--debug(inspect(gamescope.config.known_displays.steamdeck_oled_boe))
