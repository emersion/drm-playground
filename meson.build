project('drm_playground', 'c',
	version: '0.0.0',
	license: 'MIT',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

add_project_arguments('-Wno-unused-parameter', language: 'c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

executable('drm_playground',
	files([
		'drm.c',
		'main.c',
		'util.c',
	]),
	dependencies: [
		dependency('libdrm'),
		meson.get_compiler('c').find_library('m'),
	],
	install: true,
)
