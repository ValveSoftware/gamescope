wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner_path = wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner')
wayland_scanner = find_program(wayland_scanner_path, native: true)

protocols = [
	'gamescope-xwayland',
	'gamescope-pipewire',
]

foreach name : protocols
	code = custom_target(
		name + '-protocol.c',
		input: name + '.xml',
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)

	server_header = custom_target(
		name + '-protocol.h',
		input: name + '.xml',
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)

	src += [code, server_header]
endforeach
