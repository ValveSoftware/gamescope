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
    pretty_name = "ASUS ROG Ally / ROG Ally X LCD",
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
    dynamic_refresh_rates = rogally_lcd_refresh_rates,
    -- Follow the Steam Deck OLED style for modegen by varying the VFP (Vertical Front Porch)
    --
    -- Given that this display is VRR and likely has an FB/Partial FB in the DDIC:
    -- it should be able to handle this method, and it is more optimal for latency
    -- than elongating the clock.
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for ASUS ROG Ally / ROG Ally X LCD with fixed pixel clock")
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
            warn("Couldn't do refresh "..refresh.." on ASUS ROG Ally / ROG Ally X LCD")
            return base_mode
        end

        local mode = base_mode

        gamescope.modegen.adjust_front_porch(mode, vfp)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        --debug(inspect(mode))
        return mode
    end,
    matches = function(display)
        -- There are two panels used across the ROG Ally and ROG Ally X
        -- with the same timings, but the model names are in different
        -- parts of the EDID.
        local lcd_types = {
            { vendor = "TMX", model = "TL070FVXS01-0", product = 0x0002 },
            { vendor = "BOE", data_string = "TS070FHM-LU0", product = 0x0C33 },
        }

        for index, value in ipairs(lcd_types) do
            -- We only match if the vendor and product match exactly, plus either model or data_string
            if value.vendor == display.vendor and value.product == display.product then
                if (value.model and value.model == display.model)
                   or (value.data_string and value.data_string == display.data_string) then
                    debug("[rogally_lcd] Matched vendor: "..value.vendor.." model: "..(value.model or value.data_string).." product: "..value.product)
                    return 5000
                end
            end
        end

        return -1
    end
}
debug("Registered ASUS ROG Ally / ROG Ally X LCD as a known display")
--debug(inspect(gamescope.config.known_displays.rogally_lcd))
