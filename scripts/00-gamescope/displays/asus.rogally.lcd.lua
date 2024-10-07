local rogally_lcd_refresh_rates = {
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
    88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120
}

gamescope.config.known_displays.rogally_lcd = {
    pretty_name = "ROG Ally/Ally X LCD",
    hdr = {
        -- Setup some fallbacks for undocking with HDR, meant
        -- for the internal panel. It does not support HDR.
        supported = false,
        force_enabled = false,
        eotf = gamescope.eotf.gamma22,
        max_content_light_level = 500,
        max_frame_average_luminance = 500,
        min_content_light_level = 0.5
    },
    -- Use the EDID colorimetry for now, but someone may
    -- be able to tune this better for gamescope.
    dynamic_refresh_rates = rogally_lcd_refresh_rates,
    -- Follow the Steam Deck OLED style for modegen
    -- and VFPs given framebuffer capabilities.
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for ROG Ally with fixed pixel clock")
        local vfps = {
            1771, 1720, 1655, 1600, 1549,
            1499, 1455, 1405, 1361, 1320,
            1279, 1224, 1200, 1162, 1120,
            1088, 1055, 1022, 991,  958,
            930,  900,  871,  845,  817,
            794,  762,  740,  715,  690,
            668,  647,  626,  605,  585,
            566,  546,  526,  507,  488,
            470,  452,  435,  419,  402,
            387,  371,  355,  341,  326,
            310,  297,  284,  269,  255,
            244,  229,  217,  204,  194,
            181,  172,  158,  147,  136,
            123,  113,  104,  93,   83,
            74,   64,   54
        }
        local vfp = vfps[zero_index(refresh - 48)]
        if vfp == nil then
            warn("Couldn't do refresh "..refresh.." on ROG Ally")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    -- There is only a single panel model in use across both
    -- ROG Ally + ROG Ally X.
    matches = function(display)
        if display.vendor == "TMX" and display.model == "TL070FVXS01-0" and display.product == 0x0002 then
            debug("[rogally_lcd] Matched vendor: "..value.vendor.." model: "..value.model.." product:"..value.product)
            return 5000
        end
        return -1
    end
}
    debug("Registered ROG Ally/Ally X LCD as a known display")
    --debug(inspect(gamescope.config.known_displays.rogally_lcd))
