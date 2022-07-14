#!/usr/bin/env python

# Copyright 2007-2022 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: CC0-1.0 OR ISC

import os

from waflib import Build, Logs, Options, Utils
from waflib.extras import autowaf as autowaf

# Version of this package (even if built as a child)
JALV_VERSION = '1.6.7'

# Mandatory waf variables
APPNAME = 'jalv'        # Package name for waf dist
VERSION = JALV_VERSION  # Package version for waf dist
top     = '.'           # Source directory
out     = 'build'       # Build directory

# Release variables
uri          = 'http://drobilla.net/sw/jalv'
dist_pattern = 'http://download.drobilla.net/jalv-%d.%d.%d.tar.bz2'
post_tags    = ['Hacking', 'LAD', 'LV2', 'Jalv']


def options(ctx):
    ctx.load('compiler_c')
    ctx.load('compiler_cxx')
    ctx.add_flags(
        ctx.configuration_options(),
        {'portaudio':       'use PortAudio backend, not JACK',
         'no-gui':          'do not build any GUIs',
         'no-gtk':          'do not build Gtk GUI',
         'no-gtk2':         'do not build Gtk2 GUI',
         'no-gtk3':         'do not build Gtk3 GUI',
         'no-qt':           'do not build Qt GUI',
         'no-qt5':          'do not build Qt5 GUI'})


def configure(conf):
    conf.load('compiler_c', cache=True)
    conf.load('compiler_cxx', cache=True)
    conf.load('autowaf', cache=True)
    autowaf.set_c_lang(conf, 'c99')
    autowaf.set_cxx_lang(conf, 'c++14')

    if Options.options.strict:
        # Check for programs used by lint target
        conf.find_program("flake8", var="FLAKE8", mandatory=False)
        conf.find_program("clang-tidy", var="CLANG_TIDY", mandatory=False)
        conf.find_program("iwyu_tool", var="IWYU_TOOL", mandatory=False)

    if Options.options.ultra_strict:
        autowaf.add_compiler_flags(conf.env, 'c', {
            'clang': [
                '-Wno-bad-function-cast',
                '-Wno-declaration-after-statement',
                '-Wno-missing-noreturn',
            ],
            'gcc': [
                '-Wno-bad-function-cast',
                '-Wno-c++-compat',
            ],
            'msvc': [
                '/wd4061', # enumerator in switch is not explicitly handled
                '/wd4100', # unreferenced formal parameter
                '/wd4191', # unsafe function conversion
                '/wd4200', # zero-sized array in struct/union
                '/wd4244', # possible loss of data from integer conversion
                '/wd4267', # possible loss of data from size conversion
                '/wd4365', # signed/unsigned mismatch
                '/wd4514', # unreferenced inline function has been removed
                '/wd4706', # assignment within conditional expression
                '/wd4710', # function not inlined
                '/wd4711', # function selected for automatic inline expansion
                '/wd4800', # implicit conversion from int to bool
                '/wd4820', # padding added after construct
                '/wd4996', # POSIX name for this item is deprecated
                '/wd5045', # compiler will insert Spectre mitigation
            ],
        })

        autowaf.add_compiler_flags(conf.env, '*', {
            'clang': [
                '-Wno-atomic-implicit-seq-cst',
                '-Wno-c99-extensions',
                '-Wno-cast-align',
                '-Wno-cast-qual',
                '-Wno-disabled-macro-expansion',
                '-Wno-documentation-unknown-command',
                '-Wno-double-promotion',
                '-Wno-float-conversion',
                '-Wno-float-equal',
                '-Wno-format-nonliteral',
                '-Wno-implicit-fallthrough',
                '-Wno-implicit-float-conversion',
                '-Wno-nullability-extension',
                '-Wno-padded',
                '-Wno-redundant-parens',
                '-Wno-reserved-id-macro',
                '-Wno-reserved-identifier',
                '-Wno-shorten-64-to-32',
                '-Wno-sign-conversion',
                '-Wno-switch-enum',
                '-Wno-unknown-warning-option',
                '-Wno-unused-macros',
                '-Wno-unused-parameter',
            ],
            'gcc': [
                '-Wno-cast-align',
                '-Wno-cast-qual',
                '-Wno-conversion',
                '-Wno-double-promotion',
                '-Wno-float-conversion',
                '-Wno-float-equal',
                '-Wno-padded',
                '-Wno-pedantic',
                '-Wno-stack-protector',
                '-Wno-switch-enum',
                '-Wno-unused-macros',
                '-Wno-unused-parameter',
            ],
        })

        autowaf.add_compiler_flags(conf.env, 'cxx', {
            'clang': [
                '-Wno-extra-semi-stmt',
                '-Wno-old-style-cast',
                '-Wno-weak-vtables',
                '-Wno-zero-as-null-pointer-constant',
            ],
            'gcc': [
                '-Wno-effc++',
            ],
        })

    conf.check_pkg('lv2 >= 1.18.0', uselib_store='LV2')
    conf.check_pkg('lilv-0 >= 0.24.0', uselib_store='LILV')
    conf.check_pkg('serd-0 >= 0.24.0', uselib_store='SERD')
    conf.check_pkg('sord-0 >= 0.14.0', uselib_store='SORD')
    conf.check_pkg('sratom-0 >= 0.6.4', uselib_store='SRATOM')
    if Options.options.portaudio:
        conf.check_pkg('portaudio-2.0 >= 2.0.0',
                       uselib_store='PORTAUDIO',
                       system=True,
                       mandatory=False)
    else:
        conf.check_pkg('jack >= 0.120.0',
                       uselib_store='JACK',
                       system=True)

    if not Options.options.no_gui and not Options.options.no_gtk:
        if not Options.options.no_gtk2:
            conf.check_pkg('gtk+-2.0 >= 2.18.0',
                           uselib_store='GTK2',
                           system=True,
                           mandatory=False)
        if not Options.options.no_gtk3:
            conf.check_pkg('gtk+-3.0 >= 3.0.0',
                           uselib_store='GTK3',
                           system=True,
                           mandatory=False)

    if not Options.options.no_gui and not Options.options.no_qt:
        if not Options.options.no_qt5:
            conf.check_pkg('Qt5Widgets >= 5.1.0',
                           uselib_store='QT5',
                           system=True,
                           mandatory=False)
            if conf.env.HAVE_QT5:
                if not conf.find_program('moc-qt5', var='MOC5',
                                         mandatory=False):
                    conf.find_program('moc', var='MOC5')

    have_gui = (conf.env.HAVE_GTK2 or conf.env.HAVE_GTK3 or conf.env.HAVE_QT5)

    conf.check_pkg('suil-0 >= 0.10.0', uselib_store='SUIL')

    if conf.env.HAVE_JACK:
        conf.check_function(
            'c', 'jack_port_type_get_buffer_size',
            header_name = 'jack/jack.h',
            define_name = 'HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE',
            uselib      = 'JACK',
            return_type = 'size_t',
            arg_types   = 'jack_client_t*,const char*',
            mandatory   = False)

        conf.check_function('c', 'jack_set_property',
                            header_name = 'jack/metadata.h',
                            define_name = 'HAVE_JACK_METADATA',
                            uselib      = 'JACK',
                            return_type = 'int',
                            arg_types   = '''jack_client_t*,
                                             jack_uuid_t,
                                             const char*,
                                             const char*,
                                             const char*''',
                            mandatory   = False)

    defines = ['_POSIX_C_SOURCE=200809L']

    conf.env.PTHREAD_CFLAGS = []
    conf.env.PTHREAD_LINKFLAGS = []
    if conf.env.DEST_OS != 'win32':
        if conf.check(cflags=['-pthread'], mandatory=False):
            conf.env.PTHREAD_CFLAGS = ['-pthread']
        if conf.check(linkflags=['-pthread'], mandatory=False):
            if not (conf.env.DEST_OS == 'darwin'
                    and conf.env.CXX_NAME == 'clang'):
                conf.env.PTHREAD_LINKFLAGS += ['-pthread']
        elif conf.check(linkflags=['-lpthread'], mandatory=False):
            conf.env.PTHREAD_LINKFLAGS += ['-lpthread']

    conf.check_function('c', 'isatty',
                        header_name = 'unistd.h',
                        defines     = defines,
                        define_name = 'HAVE_ISATTY',
                        return_type = 'int',
                        arg_types   = 'int',
                        mandatory   = False)

    conf.check_function('c', 'fileno',
                        header_name = 'stdio.h',
                        defines     = defines,
                        define_name = 'HAVE_FILENO',
                        return_type = 'int',
                        arg_types   = 'FILE*',
                        mandatory   = False)

    conf.check_function('c', 'mlock',
                        header_name = 'sys/mman.h',
                        defines     = defines,
                        define_name = 'HAVE_MLOCK',
                        return_type = 'int',
                        arg_types   = 'const void*,size_t',
                        mandatory   = False)

    conf.check_function('c', 'sigaction',
                        header_name = 'signal.h',
                        defines     = defines,
                        define_name = 'HAVE_SIGACTION',
                        return_type = 'int',
                        arg_types   = '''int,
                                         const struct sigaction*,
                                         struct sigaction*''',
                        mandatory   = False)

    conf.define('JALV_VERSION', JALV_VERSION)
    conf.write_config_header('jalv_config.h', remove=False)

    autowaf.display_summary(
        conf,
        {'Backend': 'Jack' if conf.env.HAVE_JACK else 'PortAudio',
         'Jack metadata support': conf.is_defined('HAVE_JACK_METADATA'),
         'Gtk 2.0 support': bool(conf.env.HAVE_GTK2),
         'Gtk 3.0 support': bool(conf.env.HAVE_GTK3),
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
                  cflags       = bld.env.PTHREAD_CFLAGS,
                  linkflags    = bld.env.PTHREAD_LINKFLAGS,
                  defines      = ['ZIX_INTERNAL'],
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
              cflags       = bld.env.PTHREAD_CFLAGS,
              linkflags    = bld.env.PTHREAD_LINKFLAGS,
              defines      = ['ZIX_INTERNAL'],
              includes     = ['.', 'src'],
              uselib       = libs,
              install_path = '${BINDIR}')

    # Gtk2 version
    if bld.env.HAVE_GTK2:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk',
                  cflags       = bld.env.PTHREAD_CFLAGS,
                  linkflags    = bld.env.PTHREAD_LINKFLAGS,
                  defines      = ['ZIX_INTERNAL'],
                  includes     = ['.', 'src'],
                  lib          = ['m'],
                  uselib       = libs + ' GTK2',
                  install_path = '${BINDIR}')

    # Gtk3 version
    if bld.env.HAVE_GTK3:
        obj = bld(features     = 'c cprogram',
                  source       = source + ' src/jalv_gtk.c',
                  target       = 'jalv.gtk3',
                  cflags       = bld.env.PTHREAD_CFLAGS,
                  linkflags    = bld.env.PTHREAD_LINKFLAGS,
                  defines      = ['ZIX_INTERNAL'],
                  includes     = ['.', 'src'],
                  lib          = ['m'],
                  uselib       = libs + ' GTK3',
                  install_path = '${BINDIR}')

        bld(features         = 'subst',
            source           = 'jalv.desktop.in',
            target           = 'jalv.desktop',
            install_path     = '${DATADIR}/applications',
            chmod            = Utils.O644,
            BINDIR           = os.path.normpath(bld.env.BINDIR),
            APP_INSTALL_NAME = 'jalv.gtk3',
            APP_HUMAN_NAME   = 'Jalv')

    # Qt5 version
    if bld.env.HAVE_QT5:
        obj = bld(rule = '${MOC5} ${SRC} > ${TGT}',
                  source = 'src/jalv_qt.hpp',
                  target = 'jalv_qt5_meta.cpp')
        obj = bld(features     = 'c cxx cxxprogram',
                  source       = source + ' src/jalv_qt.cpp jalv_qt5_meta.cpp',
                  target       = 'jalv.qt5',
                  cflags       = bld.env.PTHREAD_CFLAGS,
                  linkflags    = bld.env.PTHREAD_LINKFLAGS,
                  defines      = ['ZIX_INTERNAL'],
                  includes     = ['.', 'src'],
                  uselib       = libs + ' QT5',
                  install_path = '${BINDIR}',
                  cxxflags     = ['-fPIC'])

    # Man pages
    bld.install_files('${MANDIR}/man1', bld.path.ant_glob('doc/*.1'))


class LintContext(Build.BuildContext):
    fun = cmd = 'lint'


def lint(ctx):
    "checks code for style issues"
    import os
    import subprocess
    import sys

    st = 0

    if "FLAKE8" in ctx.env:
        Logs.info("Running flake8")
        st = subprocess.call([ctx.env.FLAKE8[0],
                              "wscript",
                              "--ignore",
                              "E101,E129,W191,E221,W504,E251,E241,E741"])
    else:
        Logs.warn("Not running flake8")

    if "IWYU_TOOL" in ctx.env:
        Logs.info("Running include-what-you-use")

        qt_mapping_file = "/usr/share/include-what-you-use/qt5_11.imp"
        extra_args = []
        if os.path.exists(qt_mapping_file):
            extra_args += ["--", "-Xiwyu", "--mapping_file=" + qt_mapping_file]

        cmd = [ctx.env.IWYU_TOOL[0], "-o", "clang", "-p", "build"] + extra_args
        output = subprocess.check_output(cmd).decode('utf-8')
        if 'error: ' in output:
            sys.stdout.write(output)
            st += 1
    else:
        Logs.warn("Not running include-what-you-use")

    if "CLANG_TIDY" in ctx.env and "clang" in ctx.env.CXX[0]:
        Logs.info("Running clang-tidy")

        import json

        with open('build/compile_commands.json', 'r') as db:
            commands = json.load(db)
            files = [c['file'] for c in commands]

        for step_files in zip(*(iter(files),) * Options.options.jobs):
            procs = []
            for f in step_files:
                cmd = [ctx.env.CLANG_TIDY[0], '--quiet', '-p=.', f]
                procs += [subprocess.Popen(cmd, cwd='build')]

            for proc in procs:
                proc.communicate()
                st += proc.returncode
    else:
        Logs.warn("Not running clang-tidy")

    if st != 0:
        Logs.warn("Lint checks failed")
        sys.exit(st)


def dist(ctx):
    ctx.base_path = ctx.path
    ctx.excl = ctx.get_excl() + ' .gitmodules'
