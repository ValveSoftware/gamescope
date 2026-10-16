local rogallyx20_oled_refresh_rates = {
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
    70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
    90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120
}

gamescope.config.known_displays.rogallyx20_oled = {
    pretty_name = "ASUS ROG Xbox Ally X20 OLED",
    hdr = {
        supported = true,
        force_enabled = false,
        eotf = gamescope.eotf.pq,
        content_driven = true,
        -- From the EDID's DisplayID 2 HDR block, which libdisplay-info before 0.4.0 cannot find.
        max_content_light_level = 1405,
        max_frame_average_luminance = 800,
        min_content_light_level = 0,
    },
    dynamic_refresh_rates = rogallyx20_oled_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for ASUS ROG Xbox Ally X20 OLED with fixed pixel clock")
        local vfps = {
            3504, 3355, 3216, 3085, 2962,
            2846, 2736, 2632, 2534, 2441,
            2352, 2268, 2187, 2111, 2038,
            1968, 1901, 1837, 1776, 1717,
            1661, 1607, 1554, 1504, 1456,
            1409, 1365, 1321, 1279, 1239,
            1200, 1162, 1126, 1090, 1056,
            1023, 991,  959,  929,  899,
            871,  843,  816,  790,  764,
            739,  715,  691,  668,  646,
            624,  603,  582,  562,  542,
            522,  503,  485,  467,  449,
            432,  415,  399,  382,  367,
            351,  336,  321,  307,  292,
            278,  265,  251,  238,  225,
            213,  200,  188,  176,  164,
            153,  141,  130,  119,  109,
            98,   88,   78,   68,   58,
            48
        }
        local vfp = vfps[zero_index(refresh - 30)]
        if vfp == nil then
            warn("Couldn't do refresh "..refresh.." on ASUS ROG Xbox Ally X20 OLED")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        return mode
    end,
    matches = function(display)
        if display.vendor == "SDC" and display.product == 0x4501 then
            debug("[rogallyx20_oled] Matched vendor: "..display.vendor.." product: "..display.product)
            return 5000
        end
        return -1
    end,
}
debug("Registered ASUS ROG Xbox Ally X20 OLED as a known display")
--debug(inspect(gamescope.config.known_displays.rogallyx20_oled))
