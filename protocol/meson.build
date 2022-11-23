wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner_path = wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner')
wayland_scanner = find_program(wayland_scanner_path, native: true)
wayland_protos_dir = wayland_protos.get_variable('pkgdatadir')

protocols = {
	'gamescope-xwayland': 'gamescope-xwayland.xml',
	'gamescope-pipewire': 'gamescope-pipewire.xml',
	'gamescope-input-method': 'gamescope-input-method.xml',
	'gamescope-tearing-control-unstable-v1': 'gamescope-tearing-control-unstable-v1.xml',
	'tearing-control-v1': wayland_protos_dir / 'staging/tearing-control/tearing-control-v1.xml',
}

foreach name, xml : protocols
	code = custom_target(
		name + '-protocol.c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)

	server_header = custom_target(
		name + '-protocol.h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)

	src += [code, server_header]
endforeach
