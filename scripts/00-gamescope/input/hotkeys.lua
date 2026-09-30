-- Default hotkey configuration for gamescope
-- Users can override or disable these in ~/.config/gamescope/scripts/*.lua
-- e.g.:
--   gamescope.config.input.hotkeys["Super+F"] = nil               -- disable hotkey
--   gamescope.config.input.hotkeys["Ctrl+Alt+F"] = "fullscreen"  -- custom combo

gamescope.config.input.hotkeys["Super+F"] = "fullscreen"
gamescope.config.input.hotkeys["Super+G"] = "toggle_grab"
gamescope.config.input.hotkeys["Super+S"] = "screenshot_hotkey"

-- Scaling and filter controls (upstream parity)
gamescope.config.input.hotkeys["Super+N"] = "toggle_nearest"
gamescope.config.input.hotkeys["Super+B"] = "filter_linear"
gamescope.config.input.hotkeys["Super+U"] = "toggle_fsr"
gamescope.config.input.hotkeys["Super+Y"] = "toggle_nis"
gamescope.config.input.hotkeys["Super+I"] = "increase_sharpness"
gamescope.config.input.hotkeys["Super+O"] = "decrease_sharpness"
