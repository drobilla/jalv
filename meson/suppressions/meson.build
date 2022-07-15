# Copyright 2020-2022 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: CC0-1.0 OR ISC

# Project-specific warning suppressions.
#
# This should be used in conjunction with the generic "warnings" sibling that
# enables all reasonable warnings for the compiler.  It lives here just to keep
# the top-level meson.build more readable.

clang_common_suppressions = [
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
]

gcc_common_suppressions = [
  '-Wno-cast-align',
  '-Wno-cast-qual',
  '-Wno-conversion',
  '-Wno-double-promotion',
  '-Wno-float-conversion',
  '-Wno-float-equal',
  '-Wno-inline',
  '-Wno-padded',
  '-Wno-pedantic',
  '-Wno-stack-protector',
  '-Wno-switch-default',
  '-Wno-switch-enum',
  '-Wno-unused-macros',
  '-Wno-unused-parameter',
]

if host_machine.system() == 'darwin'
  clang_common_suppressions += [
    '-Wno-documentation', # JACK
    '-Wno-documentation-deprecated-sync', # JACK
  ]
elif host_machine.system() == 'freebsd'
  clang_common_suppressions += [
    '-Wno-c11-extensions', # isnan and friends
  ]
endif

#####
# C #
#####

if is_variable('cc')
  c_suppressions = []

  if get_option('strict')
    if cc.get_id() == 'clang'
      c_suppressions += clang_common_suppressions + [
        '-Wno-bad-function-cast',
        '-Wno-declaration-after-statement',
        '-Wno-missing-noreturn',
      ]

    elif cc.get_id() == 'gcc'
      c_suppressions += gcc_common_suppressions + [
        '-Wno-array-bounds',
        '-Wno-bad-function-cast',
        '-Wno-c++-compat',
        '-Wno-format-nonliteral',
        '-Wno-strict-overflow',
        '-Wno-suggest-attribute=const',
        '-Wno-suggest-attribute=pure',
        '-Wno-unsuffixed-float-constants',
        '-Wno-unused-const-variable',
      ]

    elif cc.get_id() == 'msvc'
      c_suppressions += [
        '/wd4061', # enumerator in switch is not explicitly handled
        '/wd4090', # different const qualifiers
        '/wd4100', # unreferenced formal parameter
        '/wd4191', # unsafe function conversion
        '/wd4200', # zero-sized array in struct/union
        '/wd4242', # possible loss of data from float conversion
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
      ]
    endif
  endif

  c_suppressions = cc.get_supported_arguments(c_suppressions)
endif

#######
# C++ #
#######

if is_variable('cpp')
  cpp_suppressions = []

  if get_option('strict')
    if cpp.get_id() == 'clang'
      cpp_suppressions = clang_common_suppressions + [
        '-Wno-extra-semi-stmt',
        '-Wno-old-style-cast',
        '-Wno-weak-vtables',
        '-Wno-zero-as-null-pointer-constant',
      ]

    elif cpp.get_id() == 'gcc'
      cpp_suppressions = gcc_common_suppressions + [
        '-Wno-effc++',
        '-Wno-strict-overflow',
        '-Wno-suggest-attribute=const',
        '-Wno-suggest-attribute=pure',
        '-Wno-unused-const-variable',
      ]
    endif
  endif

  cpp_suppressions = cpp.get_supported_arguments(cpp_suppressions)
endif
