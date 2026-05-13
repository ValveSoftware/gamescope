project('openvr', 'cpp', version : '2.15.6', license : 'BSD-3-Clause')

cmake_build_dir = meson.current_build_dir() / 'cmake_build'
cppc = meson.get_compiler('cpp')

run_command(
    'cmake',
    '-B', cmake_build_dir,
    '-S', meson.current_source_dir(),
    '-DCMAKE_BUILD_TYPE=Release',
    '-DUSE_LIBCXX=false',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    '-DBUILD_SHARED=false',
    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
    check : true
)

run_command(
    'cmake',
    '--build', cmake_build_dir,
    check : true
)

system = host_machine.system()
cpu_family = host_machine.cpu_family()

if system == 'windows'
    platform_arch = cpu_family == 'x86_64' ? 'win64' : 'win32'
elif system == 'linux'
    if cpu_family == 'aarch64'
        platform_arch = 'linuxarm64'
    elif cpu_family == 'x86_64'
        platform_arch = 'linux64'
    else
        platform_arch = 'linux32'
    endif
elif system == 'darwin'
    platform_arch = cpu_family == 'x86_64' ? 'osx64' : 'osx32'
elif system == 'android'
    if cpu_family == 'aarch64'
        platform_arch = 'androidarm64'
    else
        error('Unsupported Android architecture')
    endif
else
    error('Unsupported platform: ' + system)
endif

openvr_lib = cppc.find_library(
    'openvr_api',
    dirs : meson.current_source_dir() / 'bin' / platform_arch,
    required : true
)
openvr_dep = declare_dependency(
    dependencies : [openvr_lib],
    include_directories : include_directories('headers'),
)
meson.override_dependency('openvr', openvr_dep)
