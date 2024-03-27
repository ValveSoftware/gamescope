fs = import('fs')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner_path = wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner')
wayland_scanner = find_program(wayland_scanner_path, native: true)

wayland_protos = dependency('wayland-protocols',
	fallback: 'wayland-protocols',
	default_options: ['tests=false'],
)
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

protocols = [
	# Upstream protocols
	wl_protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
	wl_protocol_dir / 'stable/viewporter/viewporter.xml',
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	wl_protocol_dir / 'stable/presentation-time/presentation-time.xml',
	wl_protocol_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
	wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
	wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',

	# Frog protocols
	'frog-color-management-v1.xml',

	# Gamescope protocols
	'gamescope-xwayland.xml',
	'gamescope-pipewire.xml',
	'gamescope-input-method.xml',
	'gamescope-control.xml',
	'gamescope-swapchain.xml',
]

protocols_client_src = []
protocols_server_src = []

foreach xml : protocols
	name = fs.name(xml).replace('.xml', '')

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

	client_header = custom_target(
		name + '-client-protocol.h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)

	protocols_server_src += [code, server_header]
	protocols_client_src += [code, client_header]
endforeach
