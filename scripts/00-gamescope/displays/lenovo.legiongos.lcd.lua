local legiongos_lcd_refresh_rates = {
    52, 53, 54, 56, 57, 58, 59,
    60, 61, 62, 63, 64, 65, 67, 68, 69,
    70,
    102, 103, 104, 105, 106, 107, 108, 109,
    111, 112, 113, 114, 115, 116, 117, 118, 119,
    120
}

gamescope.config.known_displays.legiongos_lcd = {
    pretty_name = "Lenovo Legion Go S LCD",
    hdr = {
        -- The Legion Go S panel does not support HDR.
        supported = false,
        force_enabled = false,
            eotf = gamescope.eotf.gamma22,
            max_content_light_level = 500,
            max_frame_average_luminance = 500,
            min_content_light_level = 0.5
    },
    -- 60Hz has a different pixel clock than 120Hz in the EDID with VRR disabled,
    -- and the panel is not responsive to tuning VFPs. To cover the non-VRR
    -- limiter, an LCD Deck-style dynamic modegen method works best.
    dynamic_refresh_rates = legiongos_lcd_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for Lenovo Legion Go S LCD")
        local mode = base_mode

        -- These are only tuned for 1920x1200.
        gamescope.modegen.set_resolution(mode, 1920, 1200)

        -- hfp, hsync, hbp
        gamescope.modegen.set_h_timings(mode, 48, 36, 80)
        -- vfp, vsync, vbp
        gamescope.modegen.set_v_timings(mode, 54, 6, 4)
        mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    matches = function(display)
        local lcd_types = {
            { vendor = "CSW", model = "PN8007QB1-1", product = 0x0800 },
            { vendor = "BOE", model = "NS080WUM-LX1", product = 0x0C00 },
            { vendor = "BOE", model = "NS080WUM-LX1", product = 0x0CFF },
        }

        for index,value in ipairs(lcd_types) do
            if value.vendor == display.vendor and value.model == display.model and value.product == display.product then
                debug("[legiongos_lcd] Matched vendor: "..display.vendor.." model: "..display.model.." product: "..display.product)
                return 5000
            end
        end

        return -1
    end
}
debug("Registered Lenovo Legion Go S LCD as a known display")
--debug(inspect(gamescope.config.known_displays.legiongos_lcd))
