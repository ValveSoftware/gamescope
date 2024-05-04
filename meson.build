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
  '-Wno-ignored-qualifiers', 
  '-Wno-missing-braces',
]), language: 'cpp')

add_project_arguments(cppc.get_supported_arguments([
  '-ffast-math',
]), language: 'cpp')

pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
librt_dep = cppc.find_library('rt', required : get_option('pipewire'))
hwdata_dep = dependency('hwdata', required : false)

dep_x11 = dependency('x11')
dep_wayland = dependency('wayland-client')
vulkan_dep = dependency('vulkan')

add_project_arguments(
  '-DHAVE_PIPEWIRE=@0@'.format(pipewire_dep.found().to_int()),
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
