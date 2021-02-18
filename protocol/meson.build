wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner_path = wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner')
wayland_scanner = find_program(wayland_scanner_path, native: true)

code = custom_target(
	'gamescope-xwayland-protocol.c',
	input: 'gamescope-xwayland.xml',
	output: '@BASENAME@-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

server_header = custom_target(
	'gamescope-xwayland-protocol.h',
	input: 'gamescope-xwayland.xml',
	output: '@BASENAME@-protocol.h',
	command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

gamescope_xwayland_proto_files = [code, server_header]
