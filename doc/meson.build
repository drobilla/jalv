# Copyright 2022-2024 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

docdir = get_option('datadir') / 'doc'

if not get_option('man').disabled()
  install_man(files('jalv.1'))

  if not get_option('gtk3').disabled()
    install_man(files('jalv.gtk3.1'))
  endif

  if not get_option('qt5').disabled()
    install_man(files('jalv.qt5.1'))
  endif

  if not get_option('qt6').disabled()
    install_man(files('jalv.qt6.1'))
  endif
endif

# Build/install HTML man pages if mandoc is present
mandoc = find_program('mandoc', required: get_option('man_html'))
if mandoc.found()
  configure_file(
    copy: true,
    input: files('mandoc.css'),
    output: 'mandoc.css',
    install_dir: docdir / meson.project_name() / 'man',
  )

  mandoc_html_command = [
    mandoc,
    '-Kutf-8',
    '-Ostyle=mandoc.css,man=%N.html',
    '-Thtml',
    '-Wwarning,stop', '@INPUT@',
  ]

  html_mandir = docdir / meson.project_name() / 'man'
  foreach name : ['jalv', 'jalv.gtk3', 'jalv.qt5', 'jalv.qt6']
    custom_target(
      name + '.html',
      capture: true,
      command: mandoc_html_command,
      input: files(name + '.1'),
      install: true,
      install_dir: html_mandir,
      output: name + '.html',
    )
  endforeach

  if not meson.is_subproject()
    summary(
      'HTML man pages',
      get_option('prefix') / html_mandir,
      section: 'Directories',
    )
  endif
endif
