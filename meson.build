project('lsp', 'c',
  version : run_command('cat', '.version', check : true).stdout(),
  license : 'GPLv2+')

version = run_command('cat', '.version', check : true).stdout()

version_arg = '-DLSP_VERSION="' + version + '"'

add_global_arguments(version_arg, language : 'c')

ncursesw_dep = dependency('ncursesw')

executable(
        'lsp',
        'lsp.c',
        link_args : ['-lutil'],
        dependencies : ncursesw_dep,
        install : true
)

install_data(
        'lsp_cat',
        install_dir: 'bin',
        install_mode: 'rwxr-xr-x'
)

subdir('doc')
