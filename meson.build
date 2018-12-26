project('drm_playground', 'c',
	version: '0.0.0',
	license: 'MIT',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

add_project_arguments([
	'-Wno-unused-parameter',
	'-Wno-missing-braces',
	'-D_POSIX_C_SOURCE=200809L',
], language: 'c')

dp_inc = include_directories('include')

dp_deps = [
	dependency('libdrm'),
]

dp_lib = static_library(
	'dp',
	files([
		'drm_connector.c',
		'drm_crtc.c',
		'drm_device.c',
		'drm_plane.c',
		'drm_prop.c',
		'fb_dumb.c',
		'util.c',
	]),
	dependencies: dp_deps,
	include_directories: dp_inc,
)

dp = declare_dependency(
	link_with: dp_lib,
	dependencies: dp_deps,
	include_directories: dp_inc,
)

executable(
	'planes',
	files('planes.c'),
	dependencies: [dp],
)

executable(
	'writeback',
	files('writeback.c'),
	dependencies: [dp],
)
