project(
  'gamescope',
  ['c','cpp'],
  meson_version: '>=0.54.0',
  default_options: ['warning_level=2'],
)

add_project_arguments([
	'-DWLR_USE_UNSTABLE',
], language: ['c','cpp'])

cc = meson.get_compiler('c')
cppc = meson.get_compiler('cpp')

add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter',
]), language: 'c')

add_project_arguments(cppc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
]), language: 'cpp')

dep_x11 = dependency('x11')
dep_xdamage = dependency('xdamage')
dep_xcomposite = dependency('xcomposite')
dep_xrender = dependency('xrender')
dep_xext = dependency('xext')
dep_xfixes = dependency('xfixes')
dep_xxf86vm = dependency('xxf86vm')
dep_xtst = dependency('xtst')
dep_xres = dependency('xres')

drm_dep = dependency('libdrm')
vulkan_dep = dependency('vulkan')

cc = meson.get_compiler('c')

wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.17')
xkbcommon      = dependency('xkbcommon')
math           = cc.find_library('m')
thread_dep = dependency('threads')
cap_dep = cc.find_library('cap')
sdl_dep = dependency('SDL2')

wlroots_proj = subproject('wlroots', required: false, default_options:
  ['default_library=static', 'examples=false', 'xwayland=enabled'])
if wlroots_proj.found()
  wlroots_dep = wlroots_proj.get_variable('wlroots')
else
  wlroots_dep = dependency('wlroots', version: ['>= 0.13.0', '< 0.14.0'])
endif

wlroots_has_xwayland = wlroots_dep.get_variable(pkgconfig: 'have_xwayland', internal: 'have_xwayland') == 'true'
if not wlroots_has_xwayland
  error('Cannot use wlroots built without Xwayland support')
endif

shadercompiler = find_program('glslangValidator')

spirv_shader = custom_target('shader_target',
  output : 'composite.h',
  input : 'src/composite.comp',
  command : [
    shadercompiler, '-V', '-e', 'main', '--vn',
    'composite_spv', '@INPUT@', '-o', '@OUTPUT@'
  ],
  install : false,
)

liftoff_proj = subproject('libliftoff', required: false, default_options:
  ['default_library=static'])
if liftoff_proj.found()
  liftoff_dep = liftoff_proj.get_variable('liftoff')
else
  liftoff_dep = dependency('libliftoff')
endif

subdir('protocol')

executable(
    'gamescope',
    'src/steamcompmgr.cpp',
    'src/main.cpp',
	'src/wlserver.cpp',
	'src/drm.cpp',
	'src/sdlwindow.cpp',
	'src/vblankmanager.cpp',
	[ 'src/rendervulkan.cpp', spirv_shader ],
	gamescope_xwayland_proto_files,
    dependencies : [
        dep_x11, dep_xdamage, dep_xcomposite, dep_xrender, dep_xext, dep_xfixes,
        dep_xxf86vm, dep_xres, drm_dep, wayland_server, wayland_protos,
        xkbcommon, math, thread_dep, sdl_dep, wlroots_dep,
        vulkan_dep, liftoff_dep, dep_xtst, cap_dep
    ],
    install: true,
)
