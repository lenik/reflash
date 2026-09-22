project(
    'sdmsg',
    ['cpp', 'c'],
    version: run_command(
        'sh',
        '-c', '''
        v=$(
            command -v zfr >/dev/null 2>&1 && zfr version 2>/dev/null || true
        )
    if [ -z "$v" ] && [ -f VERSION ]; then
            v=$(head -n1 VERSION | tr -d '\r')
            v=${v#v}
        fi
        if [ -z "$v" ]; then
            v="0.0.0" # FIXED TO 0.0.0, DO NOT MODIFY
        fi
        printf '%s' "$v"
        ''',
        check: false,
    ).stdout().strip(),
    license: 'AGPL-3.0-or-later',
    default_options: [
        'cpp_std=c++17',
        'c_std=c99',
        'warning_level=2',
    ],
)

project_author = 'Lenik'
project_email = 'sdmsg@bodz.net'
project_year = 2026

asciidoctor = find_program('asciidoctor', required: true)
fs = import('fs')

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
datadir = prefix / get_option('datadir')
mandir = prefix / get_option('mandir')
localedir = prefix / get_option('localedir')
pkgdocdir = datadir / 'doc' / meson.project_name()

config_h = configuration_data()
config_h.set_quoted('PROJECT_VERSION', meson.project_version())
config_h.set_quoted('PROJECT_AUTHOR', project_author)
config_h.set_quoted('PROJECT_EMAIL', project_email)
config_h.set('PROJECT_YEAR', project_year)
config_h.set_quoted('LOCALEDIR', localedir)

configure_file(
    output: 'config.h',
    configuration: config_h,
)

bas_c_dep = dependency('bas-c', required: false)
sqlite_dep = dependency('sqlite3', required: true)
crypto_dep = dependency('libcrypto', required: true)
wx_dep = dependency('wxwidgets', version: '>=3.2', required: true, modules: ['std'])

bas_args = []
if bas_c_dep.found()
    bas_args += ['-DLOGGER_NAME=' + meson.project_name(), '-DTEXT_DOMAIN=' + meson.project_name()]
endif

inc = include_directories('src', '.')

sdmsg_sources = [
    'src/main.cpp',
    'src/options.cpp',
    'src/engine/progress.cpp',
    'src/engine/pause.cpp',
    'src/engine/massage.cpp',
    'src/engine/sha1_pass.cpp',
    'src/io/device.cpp',
    'src/io/partition.cpp',
    'src/io/block_io.cpp',
    'src/db/store.cpp',
    'src/mount/automount.cpp',
    'src/fs/detect.cpp',
    'src/fs/fat.cpp',
    'src/fs/exfat.cpp',
    'src/fs/ntfs.cpp',
    'src/fs/ext.cpp',
    'src/gui/app.cpp',
    'src/gui/main_frame.cpp',
    'src/gui/grid_panel.cpp',
]

sdmsg_deps = [sqlite_dep, crypto_dep, wx_dep]
if bas_c_dep.found()
    sdmsg_deps += [bas_c_dep]
endif

executable(
    'sdmsg',
    sdmsg_sources,
    include_directories: inc,
    dependencies: sdmsg_deps,
    cpp_args: bas_args,
    install: true,
    install_dir: bindir,
)

bash_files = [
    'sdmsg.bash',
]

foreach file : bash_files
    name = fs.stem(file)
    install_data(
        file,
        install_dir: datadir / 'bash-completion' / 'completions',
        rename: name,
    )
endforeach

install_data(
    [
        'LICENSE',
        'README.md',
        'README-zh_CN.md',
    ],
    install_dir: pkgdocdir,
)

custom_target(
    'sdmsg-man',
    input: 'man/sdmsg.adoc',
    output: 'sdmsg.1',
    command: [
        asciidoctor,
        '-b', 'manpage',
        '-a', 'project-version=' + meson.project_version(),
        '-a', 'project-year=@0@'.format(project_year),
        '-a', 'project-author=' + project_author,
        '-a', 'project-email=' + project_email,
        '-o', '@OUTPUT@',
        '@INPUT@',
    ],
    build_by_default: true,
    install: true,
    install_dir: mandir / 'man1',
)

subdir('tests')

run_target(
    'look',
    command: [
        find_program('zfr'),
        'build',
        '--look',
        '-C', meson.project_source_root(),
        '-b', meson.project_build_root(),
    ],
)

run_target(
    'install-symlinks',
    command: [
        find_program('bash'),
        meson.project_source_root() / 'scripts' / 'install-symlinks.sh',
        get_option('prefix'),
        bindir,
        datadir,
        mandir,
        meson.project_build_root(),
        meson.project_source_root(),
    ],
)

run_target(
    'uninstall-symlinks',
    command: [
        find_program('bash'),
        meson.project_source_root() / 'scripts' / 'uninstall-symlinks.sh',
        bindir,
        datadir,
        mandir,
    ],
)
