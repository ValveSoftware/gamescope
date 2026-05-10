project(
  'gamescope',
  'c',
  'cpp',
  meson_version: '>=0.58.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'force_fallback_for=wlroots,libliftoff,vkroots',
  ],
)

fallbacks = get_option('force_fallback_for')
if not (fallbacks.contains('wlroots') and fallbacks.contains('libliftoff') and fallbacks.contains('vkroots'))
  error('!!!"force_fallback_for" is missing entries!!!\n\tPlease do not remove entries from force_fallback_for if you are packaging the project.\n\tWe pull in these projects at specific commits/forks/builds for a reason.\n\tIf you are not packaging, remove this line to continue.')
endif

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
], language: 'cpp')

cppc = meson.get_compiler('cpp')

data_dir = get_option('datadir')
prefix = get_option('prefix')
lib_dir = get_option('libdir')

add_project_arguments(cppc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-c99-designator',
  '-Wno-invalid-offsetof',
  '-Wno-unused-const-variable',
  '-Wno-volatile', # glm warning
  '-Wno-deprecated-volatile',
  '-Wno-ignored-qualifiers', # reshade warning
  '-Wno-missing-braces',
]), language: 'cpp')

add_project_arguments(cppc.get_supported_arguments([
  '-fno-exceptions',
  '-ffast-math',
]), language: 'cpp')

pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
librt_dep = cppc.find_library('rt', required : get_option('pipewire'))
hwdata_dep = dependency('hwdata', required : false)

dep_x11 = dependency('x11')
dep_wayland = dependency('wayland-client')
vulkan_dep = dependency('vulkan')

glm_proj = subproject('glm')
glm_dep = glm_proj.get_variable('glm_dep')
stb_proj = subproject('stb')
stb_dep = stb_proj.get_variable('stb_dep')

if get_option('enable_openvr_support')
  openvr_dep = dependency('openvr', version: '>= 2.7', required : false)
  if not openvr_dep.found()
    cmake = import('cmake')
    openvr_var = cmake.subproject_options()
    openvr_var.add_cmake_defines({'USE_LIBCXX': false,
    #HACK: remove me when openvr supports CMake 4.0
    'CMAKE_POLICY_VERSION_MINIMUM': '3.5'})
    #ENDHACK
    openvr_var.set_override_option('warning_level', '0')
    openvr_proj = cmake.subproject('openvr', options : openvr_var)
    openvr_dep = openvr_proj.dependency('openvr_api')
  endif
else
  # null dep
  openvr_dep = dependency('', required : false)
endif

add_project_arguments(
  '-DHAVE_PIPEWIRE=@0@'.format(pipewire_dep.found().to_int()),
  '-DHAVE_OPENVR=@0@'.format(openvr_dep.found().to_int()),
  '-DSCRIPT_DIR="@0@"'.format(prefix / data_dir / 'gamescope/scripts'),
  language: 'cpp',
)

if hwdata_dep.found()
  add_project_arguments(
  '-DHWDATA_PNP_IDS="@0@"'.format(hwdata_dep.get_variable('pkgdatadir') / 'pnp.ids'),
  language: 'cpp',
  )
else
  warning('Building without hwdata pnp id support.')
endif

# Vulkan headers are installed separately from the loader (which ships the
# pkg-config file)
if not cppc.check_header('vulkan/vulkan.h', dependencies: vulkan_dep)
  error('Missing vulkan-headers')
endif

subdir('protocol')

if get_option('enable_gamescope_wsi_layer')
  subdir('layer')
endif

if get_option('enable_gamescope')
  subdir('src')
endif

# Handle default script/config/looks stuff
meson.add_install_script('default_extras_install.sh')

devenv = environment()
devenv.set('GAMESCOPE_SCRIPT_PATH', join_paths(meson.current_source_dir(), 'scripts'))
meson.add_devenv(devenv)
