# Copyright 2020-2022 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

project(
  'jalv',
  ['c', 'cpp'],
  default_options: [
    'b_ndebug=if-release',
    'buildtype=release',
    'c_std=c99',
    'cpp_std=c++14',
  ],
  license: 'ISC',
  meson_version: '>= 0.56.0',
  version: '1.6.9',
)

jalv_src_root = meson.current_source_dir()
major_version = meson.project_version().split('.')[0]
version_suffix = '@0@-@1@'.format(meson.project_name(), major_version)

#######################
# Compilers and Flags #
#######################

# Required tools
cc = meson.get_compiler('c')

# Optional C++ support
if add_languages(['cpp'], native: false, required: get_option('cxx'))
  cpp = meson.get_compiler('cpp')
endif

# Set global warning suppressions
subdir('meson/suppressions')
add_project_arguments(c_suppressions, language: ['c'])
if is_variable('cpp')
  add_project_arguments(cpp_suppressions, language: ['cpp'])
endif

#######################
# Common Dependencies #
#######################

m_dep = cc.find_library('m', required: false)

thread_dep = dependency('threads')

zix_dep = dependency(
  'zix-0',
  default_options: [
    'benchmarks=disabled',
    'docs=disabled',
    'tests=disabled',
    'tests_cpp=disabled',
  ],
  fallback: ['zix', 'zix_dep'],
  version: '>= 0.4.0',
)

serd_dep = dependency(
  'serd-0',
  default_options: [
    'docs=disabled',
    'tests=disabled',
    'tools=disabled',
  ],
  fallback: ['serd', 'serd_dep'],
  version: '>= 0.30.0',
)

sord_dep = dependency(
  'sord-0',
  default_options: [
    'docs=disabled',
    'tests=disabled',
    'tools=disabled',
  ],
  fallback: ['sord', 'sord_dep'],
  version: '>= 0.14.0',
)

lv2_dep = dependency(
  'lv2',
  default_options: [
    'docs=disabled',
    'old_headers=false',
    'plugins=disabled',
    'tests=disabled',
  ],
  fallback: ['lv2', 'lv2_dep'],
  version: '>= 1.18.0',
)

sratom_dep = dependency(
  'sratom-0',
  default_options: [
    'docs=disabled',
    'tests=disabled',
  ],
  fallback: ['sratom', 'sratom_dep'],
  version: '>= 0.6.4',
)

lilv_dep = dependency(
  'lilv-0',
  default_options: [
    'bindings_py=disabled',
    'docs=disabled',
    'tests=disabled',
    'tools=disabled',
  ],
  fallback: ['lilv', 'lilv_dep'],
  version: '>= 0.24.0',
)

suil_dep = dependency(
  'suil-0',
  default_options: [
    'docs=disabled',
    'tests=disabled',
  ],
  fallback: ['suil', 'suil_dep'],
  required: get_option('suil'),
  version: '>= 0.10.0',
)

###########
# Drivers #
###########

portaudio_dep = dependency(
  'portaudio-2.0',
  include_type: 'system',
  required: get_option('portaudio'),
  version: '>= 2.0.0',
)

jack_dep = dependency(
  'jack',
  include_type: 'system',
  required: get_option('jack'),
  version: '>= 0.120.0',
)

backend_sources = files()
if get_option('jack').enabled() and get_option('portaudio').enabled()
  error('Only one of jack and portaudio can be enabled')
elif get_option('jack').enabled()
  backend_dep = jack_dep
  backend_sources += files('src/jack.c')
elif get_option('portaudio').enabled()
  backend_dep = portaudio_dep
  backend_sources += files('src/portaudio.c')
elif jack_dep.found()
  backend_dep = jack_dep
  backend_sources += files('src/jack.c')
else
  backend_dep = portaudio_dep
  backend_sources += files('src/portaudio.c')
endif

if not backend_dep.found()
  error('No backend found, either jack or portaudio is required')
endif

##########################
# Platform Configuration #
##########################

platform_defines = [
  '-DJALV_VERSION="@0@"'.format(meson.project_version()),
]

suil_defines = ['-DHAVE_SUIL=@0@'.format(suil_dep.found().to_int())]

# Determine whether to use POSIX
no_posix = get_option('posix').disabled() or host_machine.system() == 'windows'
if no_posix
  platform_defines += ['-DJALV_NO_POSIX']
elif host_machine.system() == 'darwin'
  platform_defines += [
    '-D_DARWIN_C_SOURCE',
    '-D_POSIX_C_SOURCE=200809L',
    '-D_XOPEN_SOURCE=600',
  ]
else
  platform_defines += [
    '-D_BSD_SOURCE',
    '-D_DEFAULT_SOURCE',
    '-D_POSIX_C_SOURCE=200809L',
    '-D_XOPEN_SOURCE=600',
  ]
endif

# Build platform-specific configuration arguments
if get_option('checks').disabled()
  # Generic build without platform-specific features
  platform_defines += ['-DJALV_NO_DEFAULT_CONFIG']
elif get_option('checks').enabled()
  # Only use the features detected by the build system
  platform_defines += ['-DJALV_NO_DEFAULT_CONFIG']

  if no_posix
    platform_defines += ['-DHAVE_FILENO=0']
    platform_defines += ['-DHAVE_ISATTY=0']
    platform_defines += ['-DHAVE_MLOCK=0']
    platform_defines += ['-DHAVE_POSIX_MEMALIGN=0']
    platform_defines += ['-DHAVE_SIGACTION=0']
  else
    fileno_code = '''#include <stdio.h>
int main(void) { return fileno(stdin); }'''

    isatty_code = '''#include <unistd.h>
int main(void) { return isatty(0); }'''

    mlock_code = '''#include <sys/mman.h>
int main(void) { return mlock(0, 0); }'''

    posix_memalign_code = '''#include <stdlib.h>
int main(void) { void* mem; posix_memalign(&mem, 8, 8); }'''

    sigaction_code = '''#include <signal.h>
int main(void) { return sigaction(SIGINT, 0, 0); }'''

    platform_defines += '-DHAVE_FILENO=@0@'.format(
      cc.compiles(fileno_code, args: platform_defines, name: 'fileno').to_int(),
    )

    platform_defines += '-DHAVE_ISATTY=@0@'.format(
      cc.compiles(isatty_code, args: platform_defines, name: 'isatty').to_int(),
    )

    platform_defines += '-DHAVE_MLOCK=@0@'.format(
      cc.compiles(mlock_code, args: platform_defines, name: 'mlock').to_int(),
    )

    platform_defines += '-DHAVE_POSIX_MEMALIGN=@0@'.format(
      cc.compiles(
        posix_memalign_code,
        args: platform_defines,
        name: 'posix_memalign',
      ).to_int(),
    )

    platform_defines += '-DHAVE_SIGACTION=@0@'.format(
      cc.compiles(sigaction_code, args: platform_defines, name: 'sigaction').to_int(),
    )
  endif

  jack_metadata_code = '''#include <jack/metadata.h>
int main(void) { return !!&jack_set_property; }'''

  jack_port_type_get_buffer_size_code = '''#include <jack/jack.h>
int main(void) { return !!&jack_port_type_get_buffer_size; }'''

  platform_defines += '-DHAVE_JACK_METADATA=@0@'.format(
    cc.compiles(
      jack_metadata_code,
      args: platform_defines,
      name: 'jack_metadata',
    ).to_int(),
  )

  platform_defines += '-DHAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE=@0@'.format(
    cc.compiles(
      jack_port_type_get_buffer_size_code,
      args: platform_defines,
      name: 'jack_port_type_get_buffer_size',
    ).to_int(),
  )
endif

############
# Programs #
############

sources = (
  backend_sources + files(
    'src/control.c',
    'src/jalv.c',
    'src/log.c',
    'src/lv2_evbuf.c',
    'src/state.c',
    'src/symap.c',
    'src/worker.c',
  )
)

common_dependencies = [
  backend_dep,
  lilv_dep,
  m_dep,
  serd_dep,
  sratom_dep,
  suil_dep,
  thread_dep,
  zix_dep,
]

# Internal JACK client library
if jack_dep.found()
  shared_library(
    'jalv',
    sources + files('src/jalv_console.c'),
    c_args: c_suppressions + platform_defines + ['-DHAVE_SUIL=0'],
    dependencies: common_dependencies,
    include_directories: include_directories('src'),
    install: true,
    install_dir: get_option('prefix') / get_option('libdir') / 'jack',
    name_prefix: '',
  )
endif

# Console version
executable(
  'jalv',
  sources + files('src/jalv_console.c'),
  c_args: c_suppressions + platform_defines + suil_defines,
  dependencies: common_dependencies + [suil_dep],
  include_directories: include_directories('src'),
  install: true,
)

# Gtk 3.0 GUI version
if not get_option('gtk3').disabled()
  gdk3_dep = dependency(
    'gdk-3.0',
    include_type: 'system',
    required: get_option('gtk3'),
    version: '>= 3.12.0',
  )

  gtk3_dep = dependency(
    'gtk+-3.0',
    include_type: 'system',
    required: get_option('gtk3'),
    version: '>= 3.12.0',
  )

  if gdk3_dep.found() and gtk3_dep.found()
    config = configuration_data()
    config.set('APP_INSTALL_NAME', 'jalv.gtk3')
    config.set('APP_HUMAN_NAME', 'Jalv')
    config.set('BINDIR', get_option('prefix') / get_option('bindir'))

    configure_file(
      configuration: config,
      input: files('jalv.desktop.in'),
      install: true,
      install_dir: get_option('datadir') / 'applications',
      output: 'jalv.desktop',
    )

    executable(
      'jalv.gtk3',
      sources + files('src/jalv_gtk.c'),
      c_args: c_suppressions + platform_defines + suil_defines,
      dependencies: common_dependencies + [gdk3_dep, gtk3_dep, suil_dep],
      include_directories: include_directories('src'),
      install: true,
    )
  endif
endif

# Qt 5 GUI version
if not get_option('qt5').disabled()
  qt5_dep = dependency(
    'Qt5Widgets',
    include_type: 'system',
    required: get_option('qt5'),
    version: '>= 5.1.0',
  )

  moc = find_program('moc-qt5', required: false)
  if not moc.found()
    moc = find_program('moc', required: get_option('qt5'))
  endif

  if moc.found()
    qt_args = []
    if cpp.get_id() in ['clang', 'gcc']
      qt_args = ['-fPIC']
    endif

    jalv_qt_hpp = files(jalv_src_root / 'src' / 'jalv_qt.hpp')

    jalv_qt5_meta_cpp = custom_target(
      'jalv_qt5_meta.cpp',
      capture: true,
      command: [moc, '@INPUT@'],
      input: jalv_qt_hpp,
      output: 'jalv_qt5_meta.cpp',
    )

    executable(
      'jalv.qt5',
      sources + files('src/jalv_qt.cpp') + [jalv_qt5_meta_cpp],
      c_args: c_suppressions + platform_defines + suil_defines,
      cpp_args: cpp_suppressions + platform_defines + suil_defines + qt_args,
      dependencies: common_dependencies + [qt5_dep, suil_dep],
      include_directories: include_directories('src'),
      install: true,
    )
  endif
endif

#################
# Documentation #
#################

subdir('doc')

#########
# Tests #
#########

if not get_option('tests').disabled()
  subdir('test')
endif

if not meson.is_subproject()
  summary('Install prefix', get_option('prefix'))
  summary('Executables', get_option('prefix') / get_option('bindir'))
  summary('Man pages', get_option('prefix') / get_option('mandir'))

  summary('Backend', backend_dep.name())
endif
