local winmax2_lcd_refresh_rates = {
  40, 60
}

gamescope.config.known_displays.winmax2_lcd = {
  pretty_name = "GPD Win Max 2 LCD",
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

  dynamic_refresh_rates = winmax2_lcd_refresh_rates,
  dynamic_modegen = function(base_mode, refresh)
    return base_mode
  end,

  matches = function(display)
    if display.vendor == "JDI" and display.model == "GPD1001H" then
      debug("[winmax2_lcd] Matched vendor: "..display.vendor.." model: "..display.model.." product:"..display.product)
      return 5000
    end
  end
}
debug("Registered GPD Win Max 2 LCD as a known display")
--debug(inspect(gamescope.config.known_displays.winmax2_lcd))
