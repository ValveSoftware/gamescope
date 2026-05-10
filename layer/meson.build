vkroots_dep = dependency('vkroots')
dep_xcb = dependency('xcb')
dep_x11_xcb = dependency('x11-xcb')
wayland_client = dependency('wayland-client')

gamescope_wsi_layer = shared_library('VkLayer_FROG_gamescope_wsi_' + build_machine.cpu_family(), 'VkLayer_FROG_gamescope_wsi.cpp', protocols_client_src,
  dependencies     : [ vkroots_dep, dep_xcb, dep_x11, dep_x11_xcb, glm_dep, wayland_client ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

configure_file(
    input         : 'VkLayer_FROG_gamescope_wsi.json.in',
    output        : 'VkLayer_FROG_gamescope_wsi.' + build_machine.cpu_family() + '.json',
    configuration : {'family' : build_machine.cpu_family(), 'lib_dir' : out_lib_dir },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
)