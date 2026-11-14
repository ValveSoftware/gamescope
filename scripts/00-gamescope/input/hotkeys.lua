-- Default hotkey configuration for gamescope
-- Users can override or disable these in ~/.config/gamescope/scripts/*.lua
-- e.g.:
--   gamescope.config.input.hotkeys["Super+F"] = nil               -- disable hotkey
--   gamescope.config.input.hotkeys["Ctrl+Alt+F"] = "fullscreen"  -- custom combo

gamescope.config.input.hotkeys["Super+F"] = { action = "fullscreen", nested_only = true }
gamescope.config.input.hotkeys["Super+G"] = { action = "toggle_grab", nested_only = true }
gamescope.config.input.hotkeys["Super+S"] = { action = "screenshot_hotkey", nested_only = true }

-- Scaling and filter controls (upstream parity)
gamescope.config.input.hotkeys["Super+N"] = { action = "filter_pixel", nested_only = true }
gamescope.config.input.hotkeys["Super+B"] = { action = "filter_linear", nested_only = true }
gamescope.config.input.hotkeys["Super+U"] = { action = "toggle_fsr", nested_only = true }
gamescope.config.input.hotkeys["Super+Y"] = { action = "toggle_nis", nested_only = true }
gamescope.config.input.hotkeys["Super+I"] = { action = "increase_sharpness", nested_only = true }
gamescope.config.input.hotkeys["Super+O"] = { action = "decrease_sharpness", nested_only = true }
