-- AYANEO 3 OLED panel (EDID vendor AYA, model AYAOLED_FHD)
-- 1080x1920 portrait 10-bit OLED, up to 144Hz.
--
-- Colorimetry values are taken from the DXQ7D0023 panel specification
-- (see zotac.zone.oled.lua in gamescope) — the Zotac Zone reportedly uses
-- the same panel as the AYANEO 3 (ublue-os/bazzite#5219). The missing
-- colorimetry table is the likely reason washed-out colors persist with a
-- config that only forces HDR.

local panel_id = "ayaneo_3_oled"
local panel_name = "AYANEO 3 OLED Panel"

local panel_models = {
  { vendor = "AYA", model = "AYAOLED_FHD" },
}

local panel_colorimetry = {
  r = { x = 0.6396, y = 0.3300 },
  g = { x = 0.2998, y = 0.5996 },
  b = { x = 0.1503, y = 0.0595 },
  w = { x = 0.3095, y = 0.3095 },
}

local panel_refresh_rates = { 60, 72, 90, 120, 144 }

local panel_hdr = {
  supported = true,
  force_enabled = true,
  eotf = gamescope.eotf.gamma22,
  max_content_light_level = 993,
  max_frame_average_luminance = 400,
  min_content_light_level = 0.007,
}

gamescope.config.known_displays[panel_id] = {
  pretty_name = panel_name,

  colorimetry = panel_colorimetry,
  dynamic_refresh_rates = panel_refresh_rates,
  hdr = panel_hdr,

  dynamic_modegen = function(base_mode, refresh)
    local mode = base_mode
    debug("["..panel_id.."] Generating mode "..refresh.."Hz")

    gamescope.modegen.set_resolution(mode, 1080, 1920)

    -- Timings from the panel EDID 120/144Hz DTDs: HFP 80, HSync 44, HBP 156
    gamescope.modegen.set_h_timings(mode, 80, 44, 156)
    -- VFP 48, VSync 2, VBP 14
    gamescope.modegen.set_v_timings(mode, 48, 2, 14)

    mode.clock = gamescope.modegen.calc_max_clock(mode, refresh)
    mode.vrefresh = gamescope.modegen.calc_vrefresh(mode)

    return mode
  end,

  matches = function(display)
    for i, panel in ipairs(panel_models) do
      if panel.vendor == display.vendor and panel.model == display.model then
        debug("["..panel_id.."] Matched vendor: "..display.vendor.." model: "..display.model)
        return 5000
      end
    end

    return -1
  end,
}
debug("Registered "..panel_name.." as a known display")
