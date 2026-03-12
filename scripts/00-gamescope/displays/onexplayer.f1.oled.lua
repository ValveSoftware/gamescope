local oxp_f1_oled_colorimetry = {
	r = { x = 0.6396, y = 0.3300 },
	g = { x = 0.2998, y = 0.5996 },
	b = { x = 0.1503, y = 0.0595 },
	w = { x = 0.3095, y = 0.3095 },
}

gamescope.config.known_displays.oxp_f1_oled = {
	pretty_name = "YHB02P25 OLED",
	dynamic_refresh_rates = {
		40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
		50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
		60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
		70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
		80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
		90, 100, 110, 120, 130, 140, 144,
	},
	hdr = {
		supported = true,
		force_enabled = true,
		eotf = gamescope.eotf.gamma22,
		max_content_light_level = 687.448,
		max_frame_average_luminance = 400,
		min_content_light_level = 0.005,
	},
	colorimetry = oxp_f1_oled_colorimetry,
	dynamic_modegen = function(base_mode, refresh)
		debug("Generating mode " .. refresh .. "Hz for YHB02P25 OLED")
		local mode = base_mode

		gamescope.modegen.set_resolution(mode, 1080, 1920)
		if refresh <= 60 then
			-- EDID DTD 3: native 60Hz uses different horizontal blanking
			gamescope.modegen.set_h_timings(mode, 90, 18, 72)
		else
			-- EDID DTD 1/2: native 120/144Hz timings
			gamescope.modegen.set_h_timings(mode, 80, 44, 156)
		end
		gamescope.modegen.set_v_timings(mode, 48, 2, 14)

		mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
		mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

		return mode
	end,
	matches = function(display)
		if display.vendor == "YHB" and display.model == "YHB02P25" then
			return 5000
		end
		return -1
	end,
}
debug("Registered YHB02P25 OLED as a known display")
