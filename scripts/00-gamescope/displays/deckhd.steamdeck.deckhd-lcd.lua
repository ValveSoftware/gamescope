local steamdeck_dhd_colorimetry_spec = {
    r = { x = 0.6396, y = 0.3300 },
    g = { x = 0.2998, y = 0.5996 },
    b = { x = 0.1503, y = 0.0595 },
    w = { x = 0.3125, y = 0.3291 }
}

gamescope.config.known_displays.steamdeck_deckhd_lcd = {
    pretty_name = "Steam Deck DeckHD - Unofficial Screen Replacement",
    dynamic_refresh_rates = {
        40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
        50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
        60
    },
    hdr = {
        -- Setup some fallbacks or undocking with HDR
        -- for this display.
        supported = false,
        force_enabled = false,
        eotf = gamescope.eotf.gamma22,
        max_content_light_level = 400,
        max_frame_average_luminance = 400,
        min_content_light_level = 0.5
    },
    colorimetry = steamdeck_dhd_colorimetry_spec,
	--FIXME: Measure colorimetry parameters
    --colorimetry = steamdeck_lcd_colorimetry_measured,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for DeckHD")
        local mode = base_mode

        -- These are only tuned for 1200x1920.
        gamescope.modegen.set_resolution(mode, 1200, 1920)

        -- hfp, hsync, hbp
        gamescope.modegen.set_h_timings(mode, 40, 20, 40)
        -- vfp, vsync, vbp
        gamescope.modegen.set_v_timings(mode, 18, 2, 20)

        mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    matches = function(display)
        if display.vendor == "DHD" and display.model == "DeckHD-1200p" and display.product == 0x4001 then
            debug("[steamdeck_deckhd_lcd] Matched vendor: "..display.vendor.." model: "..display.model.." product:"..display.product)
            return 5000
        end

        return -1
    end
} 
debug("Registered Steam Deck DeckHD - Unofficial Screen Replacement as a known display")
--debug(inspect(gamescope.config.known_displays.steamdeck_deckhd_lcd))
