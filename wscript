#!/usr/bin/env python

import os
import subprocess

from waflib import Options
from waflib.extras import autowaf as autowaf

# Version of this package (even if built as a child)
JALV_VERSION = '1.6.2'

# Variables for 'waf dist'
APPNAME = 'jalv'
VERSION = JALV_VERSION

# Mandatory variables
top = '.'
out = 'build'

def options(ctx):
    ctx.load('compiler_c')
    ctx.load('compiler_cxx')
    ctx.add_flags(
        ctx.configuration_options(),
        {'portaudio':       'use PortAudio backend, not JACK',
         'no-jack-session': 'do not build JACK session support',
         'no-gui':          'do not build any GUIs',
         'no-gtk':          'do not build Gtk GUI',
         'no-gtkmm':        'do not build Gtkmm GUI',
         'no-gtk2':         'do not build Gtk2 GUI',
         'no-gtk3':         'do not build Gtk3 GUI',
         'no-qt':           'do not build Qt GUI',
         'no-qt4':          'do not build Qt4 GUI',
         'no-qt5':          'do not build Qt5 GUI'})

def configure(conf):
    conf.load('compiler_c', cache=True)
    conf.load('compiler_cxx', cache=True)
    conf.load('autowaf', cache=True)
    autowaf.set_c_lang(conf, 'c99')

    conf.check_pkg('lv2 >= 1.16.0', uselib_store='LV2')
    conf.check_pkg('lilv-0 >= 0.24.0', uselib_store='LILV')
    conf.check_pkg('serd-0 >= 0.24.0', uselib_store='SERD')
    conf.check_pkg('sord-0 >= 0.14.0', uselib_store='SORD')
    conf.check_pkg('sratom-0 >= 0.6.0', uselib_store='SRATOM')
    if Options.options.portaudio:
        conf.check_pkg('portaudio-2.0 >= 2.0.0',
                       uselib_store='PORTAUDIO', mandatory=False)
    else:
        conf.check_pkg('jack >= 0.120.0', uselib_store='JACK')

    if not Options.options.no_gui and not Options.options.no_gtk:
        if not Options.options.no_gtk2:
            conf.check_pkg('gtk+-2.0 >= 2.18.0', uselib_store='GTK2',
                           mandatory=False)
        if not Options.options.no_gtkmm:
            conf.check_pkg('gtkmm-2.4 >= 2.20.0', uselib_store='GTKMM2',
                           mandatory=False)
        if not Options.options.no_gtk3:
            conf.check_pkg('gtk+-3.0 >= 3.0.0', uselib_store='GTK3',
                           mandatory=False)

    if not Options.options.no_gui and not Options.options.no_qt:
        if not Options.options.no_qt4:
            conf.check_pkg('QtGui >= 4.0.0', uselib_store='QT4',
                           mandatory=False)
            if conf.env.HAVE_QT4:
                if not conf.find_program('moc-qt4', var='MOC4', mandatory=False):
                    conf.find_program('moc', var='MOC4')

        if not Options.options.no_qt5:
            conf.check_pkg('Qt5Widgets >= 5.1.0', uselib_store='QT5',
                           mandatory=False)
            if conf.env.HAVE_QT5:
                if not conf.find_program('moc-qt5', var='MOC5', mandatory=False):
                    conf.find_program('moc', var='MOC5')

    have_gui = (conf.env.HAVE_GTK2 or conf.env.HAVE_GTKMM2 or conf.env.HAVE_GTK3 or
                conf.env.HAVE_QT4 or conf.env.HAVE_QT5)
    if have_gui:
        conf.check_pkg('suil-0 >= 0.10.0', uselib_store='SUIL')

    if conf.env.HAVE_JACK:
        conf.check_function(
            'c', 'jack_port_type_get_buffer_size',
            header_name = 'jack/jack.h',
            define_name = 'HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE',
            uselib      = 'JACK',
            mandatory   = False)

        conf.check_function('c', 'jack_set_property',
                            header_name = 'jack/metadata.h',
                            define_name = 'HAVE_JACK_METADATA',
                            uselib      = 'JACK',
                            mandatory   = False)

    defines = ['_POSIX_C_SOURCE=200809L']

    conf.check_function('c', 'isatty',
                        header_name = 'unistd.h',
                        defines     = defines,
                        define_name = 'HAVE_ISATTY',
                        mandatory   = False)

    conf.check_function('c', 'fileno',
                        header_name = 'stdio.h',
                        defines     = defines,
                        define_name = 'HAVE_FILENO',
                        mandatory   = False)

    conf.check_function('c', 'mlock',
                        header_name = 'sys/mman.h',
                        defines     = defines,
                        define_name = 'HAVE_MLOCK',
                        mandatory   = False)

    conf.check_function('c', 'sigaction',
                        header_name = 'signal.h',
                        defines     = defines,
                        define_name = 'HAVE_SIGACTION',
                        mandatory   = False)

    if conf.is_defined('HAVE_ISATTY') and conf.is_defined('HAVE_FILENO'):
        conf.env.append_unique('CFLAGS', ['-D_POSIX_C_SOURCE=200809L'])

    if not Options.options.no_jack_session:
        conf.define('JALV_JACK_SESSION', 1)

    conf.write_config_header('jalv_config.h', remove=False)

    autowaf.display_summary(
        conf,
        {'Backend': 'Jack' if conf.env.HAVE_JACK else 'PortAudio',
         'Jack metadata support': conf.is_defined('HAVE_JACK_METADATA'),
         'Gtk 2.0 support': bool(conf.env.HAVE_GTK2),
         'Gtk 3.0 support': bool(conf.env.HAVE_GTK3),
         'Gtkmm 2.0 support': bool(conf.env.HAVE_GTKMM2),
         'Qt 4.0 support': bool(conf.env.HAVE_QT4),
         'Qt 5.0 support': bool(conf.env.HAVE_QT5),
         'Color output': bool(conf.env.JALV_WITH_COLOR)})

def build(bld):
    libs   = 'LILV SUIL JACK SERD SORD SRATOM LV2 PORTAUDIO'
    source = '''
    src/control.c
    src/jalv.c
    src/log.c
    src/lv2_evbuf.c
    src/state.c
    src/symap.c
    src/worker.c
    src/zix/ring.c
    '''

    if bld.env.HAVE_JACK:
        source += 'src/jack.c'

        # Non-GUI internal JACK client library
        obj = bld(features     = 'c cshlib',
                  source       = source + ' src/jalv_console.c',
                  target       = 'jalv',
                  includes     = ['.', 'src'],
                  lib          = ['pthread'],
                  uselib       = libs,
                  install_path = '${LIBDIR}/jack')
        obj.env.cshlib_PATTERN = '%s.so'
    elif bld.env.HAVE_PORTAUDIO:
        source += 'src/portaudio.c'

    # Non-GUI version
    obj = bld(features     = 'c cprogram',
              source       = source + ' src/jalv_console.c',
              target       = 'jalv',
              includes     = ['.', 'src'],
              lib          = ['pthread'],
              uselib       = libs,
              install_path = '${BINDIR}')

    # Gtk2 version
    if bld.env.HAVE_GTK2:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk',
                  includes     = ['.', 'src'],
                  lib          = ['pthread', 'm'],
                  uselib       = libs + ' GTK2',
                  install_path = '${BINDIR}')

    # Gtk3 version
    if bld.env.HAVE_GTK3:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk3',
                  includes     = ['.', 'src'],
                  lib          = ['pthread', 'm'],
                  uselib       = libs + ' GTK3',
                  install_path = '${BINDIR}')

    # Gtkmm version
    if bld.env.HAVE_GTKMM2:
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_gtkmm2.cpp',
                  target       = 'jalv.gtkmm',
                  includes     = ['.', 'src'],
                  lib          = ['pthread'],
                  uselib       = libs + ' GTKMM2',
                  install_path = '${BINDIR}')

    # Qt4 version
    if bld.env.HAVE_QT4:
        obj = bld(rule = '${MOC4} ${SRC} > ${TGT}',
                  source = 'src/jalv_qt.cpp',
                  target = 'jalv_qt4_meta.hpp')
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_qt.cpp',
                  target       = 'jalv.qt4',
                  includes     = ['.', 'src'],
                  lib          = ['pthread'],
                  uselib       = libs + ' QT4',
                  install_path = '${BINDIR}')

    # Qt5 version
    if bld.env.HAVE_QT5:
        obj = bld(rule = '${MOC5} ${SRC} > ${TGT}',
                  source = 'src/jalv_qt.cpp',
                  target = 'jalv_qt5_meta.hpp')
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_qt.cpp',
                  target       = 'jalv.qt5',
                  includes     = ['.', 'src'],
                  lib          = ['pthread'],
                  uselib       = libs + ' QT5',
                  install_path = '${BINDIR}',
                  cxxflags     = ['-fPIC'])

    # Man pages
    bld.install_files('${MANDIR}/man1', bld.path.ant_glob('doc/*.1'))

def upload_docs(ctx):
    import glob
    import os
    for page in glob.glob('doc/*.[1-8]'):
        os.system('mkdir -p build/doc')
        os.system('soelim %s | pre-grohtml troff -man -wall -Thtml | post-grohtml > build/%s.html' % (page, page))
        os.system('rsync -avz --delete -e ssh build/%s.html drobilla@drobilla.net:~/drobilla.net/man/' % page)

def lint(ctx):
    "checks code for style issues"
    import subprocess
    cmd = ("clang-tidy -p=. -header-filter=src/ -checks=\"*," +
           "-clang-analyzer-alpha.*," +
           "-google-readability-todo," +
           "-llvm-header-guard," +
           "-llvm-include-order," +
           "-misc-unused-parameters," +
           "-readability-else-after-return\" " +
           "$(find .. -name '*.c')")
    subprocess.call(cmd, cwd='build', shell=True)

def posts(ctx):
    path = str(ctx.path.abspath())
    autowaf.news_to_posts(
        os.path.join(path, 'NEWS'),
        {'title'        : 'Jalv',
         'description'  : autowaf.get_blurb(os.path.join(path, 'README.md')),
         'dist_pattern' : 'http://download.drobilla.net/jalv-%s.tar.bz2'},
        { 'Author' : 'drobilla',
          'Tags'   : 'Hacking, LAD, LV2, Jalv' },
        os.path.join(out, 'posts'))

def dist(ctx):
    ctx.base_path = ctx.path
    ctx.excl = ctx.get_excl() + ' .gitmodules'
