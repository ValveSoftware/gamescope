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

if not get_option('force_system_libraries')
  wlroots_proj = subproject('wlroots', required: false, default_options:
    ['default_library=static', 'examples=false'])
  if wlroots_proj.found()
    wlroots_dep = wlroots_proj.get_variable('wlroots')
    wlroots_conf = wlroots_proj.get_variable('conf_data')
    wlroots_has_xwayland = wlroots_conf.get('WLR_HAS_XWAYLAND') == 1
  endif
endif

if not is_variable('wlroots_dep')
  wlroots_dep = dependency('wlroots', version: '>= 0.11.0')
  wlroots_has_xwayland = cc.get_define('WLR_HAS_XWAYLAND', prefix: '#include <wlr/config.h>', dependencies: wlroots_dep) == '1'
endif

if not wlroots_has_xwayland
  error('Cannot use wlroots built without Xwayland support')
endif

shadercompiler = find_program('glslangValidator')

spirv_shader = custom_target('shader_target',
  output : 'composite.h',
  input : 'src/composite.comp.hlsl',
  command : [
    shadercompiler, '-V', '-e', 'main', '--vn',
    'composite_spv', '@INPUT@', '-o', '@OUTPUT@'
  ],
  install : false,
)

if not get_option('force_system_libraries')
  liftoff_proj = subproject('libliftoff', required: false, default_options:
    ['default_library=static'])
  if liftoff_proj.found() and not get_option('force_system_libraries')
    liftoff_dep = liftoff_proj.get_variable('liftoff')
  endif
endif

if not is_variable('liftoff_dep')
  liftoff_dep = dependency('libliftoff')
endif

executable(
    'gamescope',
    'src/steamcompmgr.cpp',
    'src/main.cpp',
	'src/wlserver.cpp',
	'src/drm.cpp',
	'src/sdlwindow.cpp',
	'src/vblankmanager.cpp',
	[ 'src/rendervulkan.cpp', spirv_shader ],
    dependencies : [
        dep_x11, dep_xdamage, dep_xcomposite, dep_xrender, dep_xext, dep_xfixes,
        dep_xxf86vm, dep_xres, drm_dep, wayland_server, wayland_protos,
        xkbcommon, math, thread_dep, sdl_dep, wlroots_dep,
        vulkan_dep, liftoff_dep, dep_xtst, cap_dep
    ],
    install: true,
)
