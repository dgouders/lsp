prog_a2x = find_program('a2x')

run_command(prog_a2x, '--doctype', 'manpage',
        '--format', 'manpage',
        '-a', 'lsp-version=' + meson.project_version(),
        'lsp.adoc',
        check : true)

install_man('lsp-help.1', 'lsp.1')
