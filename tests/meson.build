catch2_dep = dependency('catch2-with-main', required: true)

srcdir = '../src'

convar_unittest_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp']
	convar_unittest_src += srcdir / src
endforeach
test_convar = executable(
	'test_convar',
	convar_unittest_src + ['test_convar.cpp'],
	include_directories: [srcdir, sol2_include],
	dependencies: [catch2_dep, luajit_dep, cap_dep, drm_dep, glm_dep, wlroots_dep],
	cpp_args: gamescope_cpp_args,
)
test('convar', test_convar)

utils_process_unittest_src = []
foreach src : gamescope_core_src
	utils_process_unittest_src += srcdir / src
endforeach
test_utils_process = executable(
	'test_utils_process',
	utils_process_unittest_src + ['test_utils_process.cpp'],
	include_directories: [srcdir],
	dependencies: [catch2_dep, cap_dep],
)
reaper_test_helper = executable('reaper_test_helper', 'reaper_test_helper.cpp')
test('utils/process', test_utils_process, depends: [reaper_test_helper])
