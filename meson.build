project(
  'gamescope',
  'c',
  'cpp',
  meson_version: '>=0.58.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'force_fallback_for=wlroots,libliftoff',
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
  '-Wno-ignored-qualifiers', # reshade warning
  '-Wno-missing-braces',
]), language: 'cpp')

add_project_arguments(cppc.get_supported_arguments([
  '-ffast-math',
]), language: 'cpp')

pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
librt_dep = cppc.find_library('rt', required : get_option('pipewire'))
hwdata_dep = dependency('hwdata')

dep_x11 = dependency('x11')
dep_wayland = dependency('wayland-client')
vulkan_dep = dependency('vulkan')

if get_option('enable_openvr_support')
  openvr_dep = dependency('openvr', required : false)
  if not openvr_dep.found()
    cmake = import('cmake')
    openvr_var = cmake.subproject_options()
    openvr_var.add_cmake_defines({'USE_LIBCXX': false})
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
  '-DHWDATA_PNP_IDS="@0@"'.format(hwdata_dep.get_variable('pkgdatadir') / 'pnp.ids'),
  language: 'cpp',
)

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
