# vim: ft=meson noet
project(
	'cpplox',
	'cpp',
	version: '0.1',
	default_options: [
		'cpp_std=gnu++20',
		'warning_level=2',
	],
	meson_version: '>=0.50',
)

lox = executable('lox',
	sources: files(
		'src/ast.cpp',
		'src/main.cpp',
		'src/parser.cpp',
	),
	include_directories: include_directories(
		'src',
	),
	dependencies: [
	],
	install: true,
)
