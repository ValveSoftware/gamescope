cmake = import('cmake')
openvr_var = cmake.subproject_options()
openvr_var.add_cmake_defines(
    {
        'USE_LIBCXX' : false,
        'CMAKE_POLICY_VERSION_MINIMUM' : '3.5'
    }
)
openvr_var.set_override_option('warning_level', '0')

openvr_proj = cmake.subproject('openvr', options : openvr_var)
openvr_api = openvr_proj.dependency('openvr_api')
