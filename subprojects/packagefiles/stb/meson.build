project('stb', 'c', version : '0.1.0', license : 'MIT')

stb_dep = declare_dependency(
  include_directories : include_directories('.'),
  version             : meson.project_version()
)

meson.override_dependency('stb', stb_dep)
