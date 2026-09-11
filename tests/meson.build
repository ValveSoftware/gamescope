catch2_dep = dependency('catch2-with-main', required: true)

srcdir = '../src'
unittest_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp']
	unittest_src += srcdir / src
endforeach

gamescope_tests = executable(
	'gamescope_tests',
	unittest_src + ['test_convar.cpp', 'test_focus_placement.cpp', 'test_mangoapp_config.cpp', 'test_mangoapp_control.cpp', 'test_present_timing_schedule.cpp', 'test_process.cpp', 'test_upscale.cpp', 'test_utils_parsers.cpp', 'test_utils_string.cpp', 'test_vrclient_detect.cpp'],
	include_directories: [srcdir, sol2_include],
	dependencies: [catch2_dep, luajit_dep, cap_dep, drm_dep, glm_dep, vulkan_dep, wlroots_dep],
	cpp_args: gamescope_cpp_args,
)

reaper_test_helper = executable('reaper_test_helper', 'reaper_test_helper.cpp')

test('convar', gamescope_tests, args: ['[convar]'])
test('focus_placement', gamescope_tests, args: ['[focus_placement]'])
test('mangoapp_config', gamescope_tests, args: ['[mangoapp_config]'])
test('mangoapp_control', gamescope_tests, args: ['[mangoapp_control]'])
test('present_timing_schedule', gamescope_tests, args: ['[present_timing_schedule]'])
test('upscale', gamescope_tests, args: ['[upscale]'])
test('utils/parsers', gamescope_tests, args: ['[parsers]'])
test('utils/process', gamescope_tests, args: ['[process]'], depends: [reaper_test_helper])
test('utils/string', gamescope_tests, args: ['[string]'])
test('vrclient_detect', gamescope_tests, args: ['[vrclient_detect]'])

if get_option('enable_gamescope_wsi_layer')
	present_timing_layer_tests = executable(
		'present_timing_layer_tests',
		['test_present_timing.cpp', 'test_present_timing_layer.cpp', protocols_client_src],
		dependencies: [catch2_dep, vkroots_dep, vulkan_dep, dep_xcb, dep_x11, dep_x11_xcb, glm_dep, wayland_client],
	)
	test('present_timing', present_timing_layer_tests, args: ['[present_timing]'])
	test('present_timing_layer', present_timing_layer_tests, args: ['[present_timing_layer]'])
endif
