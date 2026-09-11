vkroots_dep = dependency('vkroots')
if not cppc.compiles('''
  #include <vulkan/vulkan.h>
  #if !defined(VK_EXT_PRESENT_TIMING_SPEC_VERSION) || VK_EXT_PRESENT_TIMING_SPEC_VERSION < 3
  #error Vulkan headers with VK_EXT_present_timing revision 3 are required
  #endif
''', dependencies: vulkan_dep, name: 'VK_EXT_present_timing revision 3')
  error('The Gamescope WSI layer requires Vulkan-Headers 1.4.335 or newer')
endif
dep_xcb = dependency('xcb')
dep_x11_xcb = dependency('x11-xcb')
wayland_client = dependency('wayland-client')

gamescope_wsi_layer = shared_library('VkLayer_FROG_gamescope_wsi_' + host_machine.cpu_family(), 'VkLayer_FROG_gamescope_wsi.cpp', protocols_client_src,
  dependencies     : [ vkroots_dep, vulkan_dep, dep_xcb, dep_x11, dep_x11_xcb, glm_dep, wayland_client ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

configure_file(
    input         : 'VkLayer_FROG_gamescope_wsi.json.in',
    output        : 'VkLayer_FROG_gamescope_wsi.' + host_machine.cpu_family() + '.json',
    configuration : {'family' : host_machine.cpu_family(), 'lib_dir' : out_lib_dir },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
    install_tag   : 'runtime'
)
