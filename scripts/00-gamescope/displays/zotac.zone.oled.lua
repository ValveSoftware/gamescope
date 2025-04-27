-- colorimetry from DXQ7D0023 PDF specification
local zotac_amoled_colorimetry = {
   r = { x = 0.6396, y = 0.3300 },
   g = { x = 0.2998, y = 0.5996 },
   b = { x = 0.1503, y = 0.0595 },
   w = { x = 0.3095, y = 0.3095 }
}

gamescope.config.known_displays.zotac_amoled = {
    pretty_name = "DXQ7D0023 AMOLED",
    dynamic_refresh_rates = {
        60, 72, 90, 120, 144
    },
    hdr = {
        supported = true,
        force_enabled = true,
        eotf = gamescope.eotf.gamma22,
        max_content_light_level = 993,
        max_frame_average_luminance = 400,
        min_content_light_level = 0.007
    },
    colorimetry = zotac_amoled_colorimetry,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for DXQ7D0023 AMOLED")
        local mode = base_mode

        gamescope.modegen.set_resolution(mode, 1080, 1920)

        -- Horizontal timings from PDF:       HFP, HSync, HBP
        gamescope.modegen.set_h_timings(mode, 80, 44, 156)
        -- Vertical timings from PDF: VFP=20, VSync=1, VBP=15
        gamescope.modegen.set_v_timings(mode, 48, 2, 14)

        mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        return mode
    end,
    matches = function(display)
        -- Match based on the EDID information
        local lcd_types = {
            { vendor = "ZDZ", model = "ZDZ0501" },
            { vendor = "DXQ", model = "DXQ7D0023" },
        }

        for index, value in ipairs(lcd_types) do
            if value.vendor == display.vendor and value.model == display.model then
                debug("[zotac_amoled] Matched vendor: "..value.vendor.." model: "..value.model)
                return 5000
            end
        end

        return -1
    end
}
debug("Registered DXQ7D0023 AMOLED as a known display")
