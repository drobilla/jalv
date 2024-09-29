# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

# Project-specific warning suppressions

warning_level = get_option('warning_level')

#####
# C #
#####

if is_variable('cc')
  c_suppressions = []

  if cc.get_id() == 'clang'
    if warning_level == 'everything'
      c_suppressions += [
        '-Wno-bad-function-cast',
        '-Wno-cast-align',
        '-Wno-cast-function-type-strict',
        '-Wno-cast-qual',
        '-Wno-declaration-after-statement',
        '-Wno-disabled-macro-expansion',
        '-Wno-documentation-unknown-command', # MacOS
        '-Wno-double-promotion',
        '-Wno-float-conversion',
        '-Wno-float-equal',
        '-Wno-implicit-float-conversion',
        '-Wno-missing-noreturn',
        '-Wno-padded',
        '-Wno-reserved-id-macro', # MacOS
        '-Wno-shorten-64-to-32',
        '-Wno-sign-conversion',
        '-Wno-switch-default',
        '-Wno-unsafe-buffer-usage',
      ]

      if not meson.is_cross_build()
        c_suppressions += [
          '-Wno-poison-system-directories',
        ]
      endif
    endif

    if warning_level in ['everything', '3']
      c_suppressions += [
        '-Wno-nullability-extension',
      ]
    endif

    if host_machine.system() == 'darwin'
      c_suppressions += [
        '-Wno-documentation', # JACK
        '-Wno-documentation-deprecated-sync', # JACK
      ]
    elif host_machine.system() == 'freebsd'
      c_suppressions += [
        '-Wno-c11-extensions', # isnan and friends
      ]
    endif

  elif cc.get_id() == 'gcc'
    if warning_level == 'everything'
      c_suppressions += [
        '-Wno-bad-function-cast',
        '-Wno-c++-compat',
        '-Wno-cast-align',
        '-Wno-cast-qual',
        '-Wno-conversion',
        '-Wno-double-promotion',
        '-Wno-float-equal',
        '-Wno-inline',
        '-Wno-padded',
        '-Wno-strict-overflow',
        '-Wno-suggest-attribute=const',
        '-Wno-suggest-attribute=pure',
        '-Wno-switch-default',
        '-Wno-unsuffixed-float-constants',
        '-Wno-unused-const-variable',
      ]
    endif

  elif cc.get_id() == 'msvc'
    if warning_level == 'everything'
      c_suppressions += [
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

  if cpp.get_id() == 'clang'
    if warning_level == 'everything'
      cpp_suppressions += [
        '-Wno-c++98-compat-pedantic',
        '-Wno-cast-align', # MacOS
        '-Wno-cast-qual', # MacOS
        '-Wno-documentation-unknown-command', # MacOS
        '-Wno-double-promotion',
        '-Wno-float-conversion',
        '-Wno-implicit-float-conversion',
        '-Wno-old-style-cast', # MacOS
        '-Wno-padded',
        '-Wno-redundant-parens',
        '-Wno-reserved-id-macro', # MacOS
        '-Wno-shorten-64-to-32',
        '-Wno-sign-conversion',
        '-Wno-unsafe-buffer-usage',
        '-Wno-weak-vtables',
        '-Wno-zero-as-null-pointer-constant', # MacOS
      ]

      if not meson.is_cross_build()
        cpp_suppressions += [
          '-Wno-poison-system-directories',
        ]
      endif
    endif

    if warning_level in ['everything', '3']
      cpp_suppressions += [
        '-Wno-nullability-extension',
      ]
    endif

  elif cpp.get_id() == 'gcc'
    if warning_level == 'everything'
      cpp_suppressions += [
        '-Wno-cast-align', # LV2
        '-Wno-cast-qual', # LV2
        '-Wno-conversion',
        '-Wno-effc++',
        '-Wno-padded',
        '-Wno-strict-overflow',
        '-Wno-suggest-attribute=const',
        '-Wno-suggest-attribute=pure',
        '-Wno-unused-const-variable',
      ]
    endif
  endif

  cpp_suppressions = cpp.get_supported_arguments(cpp_suppressions)
endif
