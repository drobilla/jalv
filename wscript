#!/usr/bin/env python
import os
import sys
import subprocess

from waflib.extras import autowaf as autowaf
import waflib.Options as Options
import waflib.Logs as Logs

# Version of this package (even if built as a child)
JALV_VERSION       = '0.0.0'
JALV_MAJOR_VERSION = '0'

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
    opt.add_option('--no-jack-session', action='store_true', default=False,
                   dest='no_jack_session',
                   help="Do not build JACK session support")

def configure(conf):
    conf.load('compiler_c')
    conf.load('compiler_cxx')
    conf.line_just = 46
    autowaf.configure(conf)
    autowaf.display_header('Jalv Configuration')

    autowaf.check_pkg(conf, 'lv2core', uselib_store='LV2CORE', mandatory=True)
    autowaf.check_pkg(conf, 'lilv-0', uselib_store='LILV',
                      atleast_version='0.4.0', mandatory=True)
    autowaf.check_pkg(conf, 'serd-0', uselib_store='SERD',
                      atleast_version='0.4.5', mandatory=True)
    autowaf.check_pkg(conf, 'suil-0', uselib_store='SUIL',
                      atleast_version='0.4.0', mandatory=True)
    autowaf.check_pkg(conf, 'jack', uselib_store='JACK',
                      atleast_version='0.120.0', mandatory=True)
    autowaf.check_pkg(conf, 'gtk+-2.0', uselib_store='GTK2',
                      atleast_version='2.18.0', mandatory=False)
    autowaf.check_pkg(conf, 'QtGui', uselib_store='QT4',
                      atleast_version='4.0.0', mandatory=False)

    autowaf.check_pkg(conf, 'lv2core', uselib_store='LV2CORE')
    autowaf.check_pkg(conf, 'lv2-lv2plug.in-ns-ext-event',
                      uselib_store='LV2_EVENT')
    autowaf.check_pkg(conf, 'lv2-lv2plug.in-ns-ext-atom',
                      uselib_store='LV2_ATOM')
    autowaf.check_pkg(conf, 'lv2-lv2plug.in-ns-ext-uri-map',
                      uselib_store='LV2_URI_MAP')
    autowaf.check_pkg(conf, 'lv2-lv2plug.in-ns-ext-state',
                      uselib_store='LV2_STATE', mandatory=False)
    autowaf.check_pkg(conf, 'lv2-lv2plug.in-ns-ext-ui-resize',
                      uselib_store='LV2_UI_RESIZE', mandatory=False)

    if not Options.options.no_jack_session:
        autowaf.define(conf, 'JALV_JACK_SESSION', 1)

    conf.env.append_unique('CFLAGS', '-std=c99')
    autowaf.define(conf, 'JALV_VERSION', JALV_VERSION)

    conf.write_config_header('jalv-config.h', remove=False)

    autowaf.display_msg(conf, "LV2 State support",
                        conf.is_defined('HAVE_LV2_STATE'))
    autowaf.display_msg(conf, "Gtk 2.0 support",
                        conf.is_defined('HAVE_GTK2'))
    autowaf.display_msg(conf, "Qt 4.0 support",
                        conf.is_defined('HAVE_QT4'))
    print('')

def build(bld):
    libs = 'LILV SUIL JACK SERD LV2CORE LV2_EVENT LV2_ATOM LV2_URI_MAP LV2_STATE'

    source = 'src/jalv.c src/symap.c src/state.c src/lv2_evbuf.c'

    # Non-GUI version
    obj = bld(features     = 'c cprogram',
              source       = source + ' src/jalv_console.c',
              target       = 'jalv',
              includes     = ['.'],
              lib          = ['pthread'],
              install_path = '${BINDIR}')
    autowaf.use_lib(bld, obj, libs)

    # Gtk version
    if bld.is_defined('HAVE_GTK2'):
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk2.c',
                  target       = 'jalv.gtk',
                  includes     = ['.'],
                  lib          = ['pthread'],
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' GTK2')

    # Qt version
    if bld.is_defined('HAVE_QT4'):
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_qt4.cpp',
                  target       = 'jalv.qt',
                  includes     = ['.'],
                  lib          = ['pthread'],
                  install_path = '${BINDIR}')
        autowaf.use_lib(bld, obj, libs + ' QT4')

    # Man pages
    bld.install_files('${MANDIR}/man1', bld.path.ant_glob('doc/*.1'))

def lint(ctx):
    subprocess.call('cpplint.py --filter=+whitespace/comments,-whitespace/tab,-whitespace/braces,-whitespace/labels,-build/header_guard,-readability/casting,-readability/todo,-build/include src/* jalv/*', shell=True)
