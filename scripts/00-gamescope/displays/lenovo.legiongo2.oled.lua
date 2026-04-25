local legiongo2_oled_refresh_rates = {
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
    88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
    138, 139, 140, 141, 142, 143, 144
}

gamescope.config.known_displays.lenovo_legiongo2_oled = {
    pretty_name = "Lenovo Legion Go 2 OLED",
    hdr = {
        supported = true,
        force_enabled = false,
        eotf = gamescope.eotf.pq,
        content_driven = true,
    },
    dynamic_refresh_rates = legiongo2_oled_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for Lenovo Legion Go 2 OLED with fixed pixel clock")
        local vfps = {
            2696, 2615, 2538, 2463, 2391,
            2322, 2256, 2192, 2130, 2071,
            2013, 1958, 1904, 1852, 1802,
            1753, 1706, 1660, 1616, 1572,
            1531, 1491, 1451, 1413, 1376,
            1340, 1305, 1270, 1237, 1205,
            1173, 1142, 1112, 1083, 1054,
            1026, 999,  972,  946,  921,
            896,  872,  848,  825,  802,
            780,  758,  737,  716,  696,
            676,  656,  637,  618,  600,
            581,  564,  546,  529,  512,
            496,  480,  464,  448,  433,
            418,  403,  389,  375,  361,
            347,  333,  320,  307,  294,
            281,  269,  257,  245,  233,
            221,  209,  198,  187,  176,
            165,  155,  144,  134,  123,
            113,  103,  94,   84,   75,
            65,   56
        }
        local vfp = vfps[zero_index(refresh - 48)]
        if vfp == nil then
            warn("Couldn't do refresh "..refresh.." on Lenovo Legion Go 2 OLED")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        return mode
    end,
    matches = function(display)
        if display.vendor == "SDC" and display.product == 17153 then
            debug("[lenovo_legiongo2_oled] Matched vendor: "..display.vendor.." product: "..display.product)
            return 5000
        end
        return -1
    end,
}
debug("Registered Lenovo Legion Go 2 OLED as a known display")
--debug(inspect(gamescope.config.known_displays.lenovo_legiongo2_oled))
