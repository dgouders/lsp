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
        dependencies : ncursesw_dep,
        install : true
)

subdir('doc')
