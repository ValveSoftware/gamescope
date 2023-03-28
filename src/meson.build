dep_xdamage = dependency('xdamage')
dep_xcomposite = dependency('xcomposite')
dep_xrender = dependency('xrender')
dep_xext = dependency('xext')
dep_xfixes = dependency('xfixes')
dep_xxf86vm = dependency('xxf86vm')
dep_xtst = dependency('xtst')
dep_xres = dependency('xres')
dep_xmu = dependency('xmu')

drm_dep = dependency('libdrm', version: '>= 2.4.113')

wayland_server = dependency('wayland-server', version: '>=1.21')
wayland_protos = dependency('wayland-protocols', version: '>=1.17')
xkbcommon = dependency('xkbcommon')
thread_dep = dependency('threads')
cap_dep = dependency('libcap', required: false)
glm_dep = dependency('glm')
sdl_dep = dependency('SDL2')
stb_dep = dependency('stb')

wlroots_dep = dependency(
  'wlroots',
  version: ['>= 0.16.0', '< 0.17.0'],
  fallback: ['wlroots', 'wlroots'],
  default_options: ['default_library=static', 'examples=false', 'xwayland=enabled', 'backends=libinput', 'renderers=[]', 'allocators=[]'],
)

displayinfo_dep = dependency(
  'libdisplay-info',
  version: ['>= 0.0.0', '< 0.2.0'],
  fallback: ['libdisplay-info', 'di_dep'],
  default_options: ['default_library=static'],
)

required_wlroots_features = ['xwayland', 'libinput_backend']
foreach feat : required_wlroots_features
  if wlroots_dep.get_variable('have_' + feat) != 'true'
    error('Cannot use wlroots built without ' + feat + ' support')
  endif
endforeach

glsl_compiler = find_program('glslangValidator', native: true)

# Use --depfile to rebuild shaders when included files have changed. Sadly debian based
# distros don't have up-to-date glslang so we need to check for support first.
if run_command(glsl_compiler, ['--version', '--depfile', 'dummy.dep'], check: false).returncode() == 0
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '--quiet'],
    depfile   : '@BASENAME@.h.d',
  )
else
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@'],
  )
endif

shader_src = [
  'shaders/cs_composite_blit.comp',
  'shaders/cs_composite_blur.comp',
  'shaders/cs_composite_blur_cond.comp',
  'shaders/cs_composite_rcas.comp',
  'shaders/cs_easu.comp',
  'shaders/cs_easu_fp16.comp',
  'shaders/cs_gaussian_blur_horizontal.comp',
  'shaders/cs_nis.comp',
  'shaders/cs_nis_fp16.comp',
  'shaders/cs_rgb_to_nv12.comp',
]

spirv_shaders = glsl_generator.process(shader_src)

liftoff_dep = dependency(
  'libliftoff',
  version: ['>= 0.4.0', '< 0.5.0'],
  fallback: ['libliftoff', 'liftoff'],
  default_options: ['default_library=static'],
)


src = [
  'steamcompmgr.cpp',
  'color_helpers.cpp',
  'main.cpp',
  'wlserver.cpp',
  'drm.cpp',
  'modegen.cpp',
  'sdlwindow.cpp',
  'vblankmanager.cpp',
  'rendervulkan.cpp',
  'log.cpp',
  'ime.cpp',
  'mangoapp.cpp',
]

src += spirv_shaders
src += protocols_server_src

if pipewire_dep.found()
  src += 'pipewire.cpp'
endif

if openvr_dep.found()
  src += 'vr_session.cpp'
endif

  executable(
    'gamescope',
    src,
    dependencies: [
      dep_x11, dep_xdamage, dep_xcomposite, dep_xrender, dep_xext, dep_xfixes,
      dep_xxf86vm, dep_xres, glm_dep, drm_dep, wayland_server,
      xkbcommon, thread_dep, sdl_dep, wlroots_dep,
      vulkan_dep, liftoff_dep, dep_xtst, dep_xmu, cap_dep, pipewire_dep, librt_dep,
      stb_dep, displayinfo_dep, openvr_dep,
    ],
    install: true,
  )
