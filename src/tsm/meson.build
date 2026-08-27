# SPDX-License-Identifier: MIT

libtsm_srcs = [
    'tsm-render.c',
    'tsm-screen.c',
    'tsm-selection.c',
    'tsm-unicode.c',
    'tsm-vte-charsets.c',
    'tsm-vte.c',
]

version=meson.project_version()
major=version.split('.')[0]

cc = meson.get_compiler('c')

# Export symbols with a version script where the linker supports it.  MSVC
# uses the __declspec(dllexport) annotations from libtsm.h instead; passing
# the Unix fallback option to link.exe only produces a fatal warning under
# /WX.
link_flags = []
if cc.get_id() != 'msvc' and cc.get_id() != 'clang-cl'
    link_flags = '-Wl,--version-script=' + meson.current_source_dir() + '/libtsm.sym'
    code = 'int main() { return 0; }'

    if not cc.links(code, args: link_flags, name: '-Wl,--version-script=...')
        link_flags = [ '-export-symbols-regex=^tsm_' ]
        if not cc.links(code, args: link_flags, name: '-Wl,--export-symbol-regex')
            link_flags = []
        endif
    endif
endif

deps = [wcwidth_dep, shl_dep, xkbcommon_dep]

if get_option('default_library') == 'static'
    # Consumers of a static library must not import its symbols on MSVC.
    libtsm_c_args = ['-DTSM_STATIC']
else
    libtsm_c_args = ['-DTSM_BUILDING_DLL']
endif

# Let Meson's default_library option select the linkage model. This keeps the
# normal shared-library build while allowing embedders to use a static
# fallback without carrying a second runtime library.
libtsm = library(
    'tsm',
    libtsm_srcs,
    dependencies: deps,
    c_args: libtsm_c_args,
    install: true,
    version: version,
    soversion: major,
    link_args: link_flags,
)

libtsm_dep = declare_dependency(
    include_directories: '.',
    link_with: libtsm,
    compile_args: libtsm_c_args,
)

install_headers('libtsm.h')

pkg = import('pkgconfig')
pkg.generate(
    libtsm,
    name: 'libtsm',
    url: 'https://github.com/Aetf/libtsm',
    description: 'terminal-emulator state machine',
)
