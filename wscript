#!/usr/bin/env python
import os
import subprocess
from waflib.extras import autowaf as autowaf
import waflib.Options as Options

# Version of this package (even if built as a child)
JALV_VERSION = '1.6.1'

# Variables for 'waf dist'
APPNAME = 'jalv'
VERSION = JALV_VERSION

# Mandatory variables
top = '.'
out = 'build'

def options(opt):
    opt.load('compiler_c')
    opt.load('compiler_cxx')
    autowaf.set_options(opt)
    opt.add_option('--portaudio', action='store_true', default=False,
                   dest='portaudio',
                   help='Use PortAudio backend, not JACK')
    opt.add_option('--no-jack-session', action='store_true', default=False,
                   dest='no_jack_session',
                   help='Do not build JACK session support')
    opt.add_option('--no-gtk', action='store_true', default=False,
                   dest='no_gtk',
                   help='Do not build Gtk GUI')
    opt.add_option('--no-gtkmm', action='store_true', default=False,
                   dest='no_gtkmm',
                   help='Do not build Gtkmm GUI')
    opt.add_option('--no-gtk2', action='store_true', dest='no_gtk2',
                   help='Do not build Gtk2 GUI')
    opt.add_option('--no-gtk3', action='store_true', dest='no_gtk3',
                   help='Do not build Gtk3 GUI')
    opt.add_option('--no-qt', action='store_true', default=False, dest='no_qt',
                   help='Do not build Qt GUI')
    opt.add_option('--no-qt4', action='store_true', dest='no_qt4',
                   help='Do not build Qt4 GUI')
    opt.add_option('--no-qt5', action='store_true', dest='no_qt5',
                   help='Do not build Qt5 GUI')

def configure(conf):
    conf.line_just = 52
    conf.load('compiler_c')
    conf.load('compiler_cxx')
    autowaf.configure(conf)
    autowaf.display_header('Jalv Configuration')
    autowaf.set_c99_mode(conf)
    autowaf.set_cxx11_mode(conf)

    autowaf.check_pkg(conf, 'lv2', atleast_version='1.14.0', uselib_store='LV2')
    autowaf.check_pkg(conf, 'lilv-0', uselib_store='LILV',
                      atleast_version='0.24.0', mandatory=True)
    autowaf.check_pkg(conf, 'serd-0', uselib_store='SERD',
                      atleast_version='0.14.0', mandatory=True)
    autowaf.check_pkg(conf, 'sord-0', uselib_store='SORD',
                      atleast_version='0.12.0', mandatory=True)
    autowaf.check_pkg(conf, 'suil-0', uselib_store='SUIL',
                      atleast_version='0.8.7', mandatory=True)
    autowaf.check_pkg(conf, 'sratom-0', uselib_store='SRATOM',
                      atleast_version='0.6.0', mandatory=True)
    if Options.options.portaudio:
        autowaf.check_pkg(conf, 'portaudio-2.0', uselib_store='PORTAUDIO',
                          atleast_version='2.0.0', mandatory=False)
    else:
        autowaf.check_pkg(conf, 'jack', uselib_store='JACK',
                          atleast_version='0.120.0', mandatory=True)

    if not Options.options.no_gtk:
        if not Options.options.no_gtk2:
            autowaf.check_pkg(conf, 'gtk+-2.0', uselib_store='GTK2',
                              atleast_version='2.18.0', mandatory=False)
        if not Options.options.no_gtkmm:
            autowaf.check_pkg(conf, 'gtkmm-2.4', uselib_store='GTKMM2',
                              atleast_version='2.20.0', mandatory=False)
        if not Options.options.no_gtk3:
            autowaf.check_pkg(conf, 'gtk+-3.0', uselib_store='GTK3',
                              atleast_version='3.0.0', mandatory=False)

    if not Options.options.no_qt:
        if not Options.options.no_qt4:
            autowaf.check_pkg(conf, 'QtGui', uselib_store='QT4',
                              atleast_version='4.0.0', mandatory=False)
            if conf.env.HAVE_QT4:
                if not conf.find_program('moc-qt4', var='MOC4', mandatory=False):
                    conf.find_program('moc', var='MOC4')

        if not Options.options.no_qt5:
            autowaf.check_pkg(conf, 'Qt5Widgets', uselib_store='QT5',
                              atleast_version='5.1.0', mandatory=False)
            if conf.env.HAVE_QT5:
                if not conf.find_program('moc-qt5', var='MOC5', mandatory=False):
                    conf.find_program('moc', var='MOC5')

    if conf.env.HAVE_JACK:
        conf.check(function_name='jack_port_type_get_buffer_size',
                   header_name='jack/jack.h',
                   define_name='HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE',
                   uselib='JACK',
                   mandatory=False)

        conf.check(function_name='jack_set_property',
                   header_name='jack/metadata.h',
                   define_name='HAVE_JACK_METADATA',
                   uselib='JACK',
                   mandatory=False)

    defines = ['_POSIX_C_SOURCE=200809L']

    conf.check(function_name='isatty',
               header_name='unistd.h',
               defines=defines,
               define_name='HAVE_ISATTY',
               mandatory=False)

    conf.check(function_name='fileno',
               header_name='stdio.h',
               defines=defines,
               define_name='HAVE_FILENO',
               mandatory=False)

    conf.check(function_name='mlock',
               header_name='sys/mman.h',
               defines=defines,
               define_name='HAVE_MLOCK',
               mandatory=False)

    if conf.is_defined('HAVE_ISATTY') and conf.is_defined('HAVE_FILENO'):
        autowaf.define(conf, 'JALV_WITH_COLOR', 1)
        conf.env.append_unique('CFLAGS', ['-D_POSIX_C_SOURCE=200809L'])

    if not Options.options.no_jack_session:
        autowaf.define(conf, 'JALV_JACK_SESSION', 1)

    autowaf.define(conf, 'JALV_VERSION', JALV_VERSION)

    conf.write_config_header('jalv_config.h', remove=False)

    autowaf.display_msg(conf, "Backend", "Jack" if conf.env.HAVE_JACK else "PortAudio")
    if conf.env.HAVE_JACK:
        autowaf.display_msg(conf, "Jack metadata support",
                            conf.is_defined('HAVE_JACK_METADATA'))
    autowaf.display_msg(conf, "Gtk 2.0 support", bool(conf.env.HAVE_GTK2))
    autowaf.display_msg(conf, "Gtk 3.0 support", bool(conf.env.HAVE_GTK3))
    autowaf.display_msg(conf, "Gtkmm 2.0 support", bool(conf.env.HAVE_GTKMM2))
    autowaf.display_msg(conf, "Qt 4.0 support", bool(conf.env.HAVE_QT4))
    autowaf.display_msg(conf, "Qt 5.0 support", bool(conf.env.HAVE_QT5))
    autowaf.display_msg(conf, "Color output", bool(conf.env.JALV_WITH_COLOR))
    print('')

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
    elif bld.env.HAVE_PORTAUDIO:
        source += 'src/portaudio.c'

    # Non-GUI version
    obj = bld(features     = 'c cprogram',
              source       = source + ' src/jalv_console.c',
              target       = 'jalv',
              includes     = ['.', 'src'],
              lib          = ['pthread'],
              install_path = '${BINDIR}')
    autowaf.use_lib(bld, obj, libs)

    # Gtk2 version
    if bld.env.HAVE_GTK2:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk',
                  includes     = ['.', 'src'],
                  lib          = ['pthread', 'm'],
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' GTK2')

    # Gtk3 version
    if bld.env.HAVE_GTK3:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk3',
                  includes     = ['.', 'src'],
                  lib          = ['pthread', 'm'],
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' GTK3')

    # Gtkmm version
    if bld.env.HAVE_GTKMM2:
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_gtkmm2.cpp',
                  target       = 'jalv.gtkmm',
                  includes     = ['.', 'src'],
                  lib          = ['pthread'],
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' GTKMM2')

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
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' QT4')

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
                  install_path = '${BINDIR}',
                  cxxflags     = ['-fPIC'])
        autowaf.use_lib(bld, obj, libs + ' QT5')

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
    subprocess.call('cpplint.py --filter=+whitespace/comments,-whitespace/tab,-whitespace/braces,-whitespace/labels,-build/header_guard,-readability/casting,-readability/todo,-build/include,-runtime/sizeof src/* jalv/*', shell=True)

def posts(ctx):
    path = str(ctx.path.abspath())
    autowaf.news_to_posts(
        os.path.join(path, 'NEWS'),
        {'title'        : 'Jalv',
         'description'  : autowaf.get_blurb(os.path.join(path, 'README')),
         'dist_pattern' : 'http://download.drobilla.net/jalv-%s.tar.bz2'},
        { 'Author' : 'drobilla',
          'Tags'   : 'Hacking, LAD, LV2, Jalv' },
        os.path.join(out, 'posts'))
