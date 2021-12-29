project(
  'gamescope',
  'c',
  'cpp',
  meson_version: '>=0.58.0',
  default_options: [
    'cpp_std=c++14',
    'warning_level=2',
    'force_fallback_for=wlroots,libliftoff',
  ],
)

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
], language: 'cpp')

cppc = meson.get_compiler('cpp')

add_project_arguments(cppc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-c99-designator',
  '-Wno-invalid-offsetof'
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

drm_dep = dependency('libdrm', version: '>= 2.4.105')
vulkan_dep = dependency('vulkan')

wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.17')
xkbcommon = dependency('xkbcommon')
thread_dep = dependency('threads')
cap_dep = dependency('libcap')
sdl_dep = dependency('SDL2')
pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
librt_dep = cppc.find_library('rt', required : get_option('pipewire'))

stb_dep = dependency('stb')

wlroots_dep = dependency(
  'wlroots',
  version: ['>= 0.15.0', '< 0.16.0'],
  fallback: ['wlroots', 'wlroots'],
  default_options: ['default_library=static', 'examples=false', 'xwayland=enabled', 'backends=libinput', 'renderers=[]'],
)

required_wlroots_features = ['xwayland', 'libinput_backend']
foreach feat : required_wlroots_features
  if wlroots_dep.get_variable('have_' + feat) != 'true'
    error('Cannot use wlroots built without ' + feat + ' support')
  endif
endforeach

# Vulkan headers are installed separately from the loader (which ships the
# pkg-config file)
if not cppc.check_header('vulkan/vulkan.h', dependencies: vulkan_dep)
  error('Missing vulkan-headers')
endif


glsl_compiler = find_program('glslangValidator', native: true)

# Use --depfile to rebuild shaders when included files have changed. Sadly debian based
# distros don't have up-to-date glslang so we need to check for support first.
if run_command(glsl_compiler, ['--version', '--depfile', 'dummy.dep'], check: false).returncode() == 0
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '--quiet'],
    depfile   : '@DEPFILE@',
  )
else
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@'],
  )
endif

shader_src = [
  'src/shaders/cs_composite_blit.comp',
  'src/shaders/cs_composite_rcas.comp',
  'src/shaders/cs_easu.comp',
  'src/shaders/cs_easu_fp16.comp',
]

spirv_shaders = glsl_generator.process(shader_src)

liftoff_dep = dependency(
  'libliftoff',
  version: ['>= 0.2.0', '< 0.3.0'],
  fallback: ['libliftoff', 'liftoff'],
  default_options: ['default_library=static'],
)

add_project_arguments(
  '-DHAVE_PIPEWIRE=@0@'.format(pipewire_dep.found().to_int()),
  language: 'cpp',
)

src = [
  'src/steamcompmgr.cpp',
  'src/main.cpp',
  'src/wlserver.cpp',
  'src/drm.cpp',
  'src/modegen.cpp',
  'src/sdlwindow.cpp',
  'src/vblankmanager.cpp',
  'src/rendervulkan.cpp',
  'src/log.cpp',
  'src/ime.cpp',
  'src/mangoapp.cpp',
]

src += spirv_shaders

if pipewire_dep.found()
  src += 'src/pipewire.cpp'
endif

subdir('protocol')

executable(
  'gamescope',
  src,
  dependencies: [
    dep_x11, dep_xdamage, dep_xcomposite, dep_xrender, dep_xext, dep_xfixes,
    dep_xxf86vm, dep_xres, drm_dep, wayland_server, wayland_protos,
    xkbcommon, thread_dep, sdl_dep, wlroots_dep,
    vulkan_dep, liftoff_dep, dep_xtst, cap_dep, pipewire_dep, librt_dep, stb_dep,
  ],
  install: true,
)
