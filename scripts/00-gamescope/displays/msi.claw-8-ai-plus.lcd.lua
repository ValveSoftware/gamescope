local msi_claw_8_ai_plus_lcd_refresh_rates = {
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
    88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120
}

gamescope.config.known_displays.msi_claw_8_ai_plus_lcd = {
    pretty_name = "MSI Claw 8 AI+ LCD",
    hdr = {
        supported = false,
        force_enabled = false,
        eotf = gamescope.eotf.gamma22,
        max_content_light_level = 500,
        max_frame_average_luminance = 500,
        min_content_light_level = 0.5
    },
    dynamic_refresh_rates = msi_claw_8_ai_plus_lcd_refresh_rates,
    dynamic_modegen = function(base_mode, refresh)
        debug("Generating mode "..refresh.."Hz for MSI Claw 8 AI+ LCD")
        local mode = base_mode

        gamescope.modegen.set_resolution(mode, 1920, 1200)
        gamescope.modegen.set_h_timings(mode, 48, 32, 80)
        gamescope.modegen.set_v_timings(mode, 54, 6, 4)
        mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
        mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

        return mode
    end,
    matches = function(display)
        local lcd_types = {
            { vendor = "CSW", model = "PN8007QB1-2", product = 0x0801 },
        }

        for index, value in ipairs(lcd_types) do
            if value.vendor == display.vendor and value.model == display.model and value.product == display.product then
                debug("[msi_claw_8_ai_plus_lcd] Matched vendor: "..display.vendor.." model: "..display.model.." product: "..display.product)
                return 5000
            end
        end

        return -1
    end
}
debug("Registered MSI Claw 8 AI+ LCD as a known display")
