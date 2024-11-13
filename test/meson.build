# Copyright 2019-2024 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

########
# Lint #
########

all_sources = (
  common_sources + files(
    '../src/attributes.h',
    '../src/backend.h',
    '../src/control.h',
    '../src/frontend.h',
    '../src/jack.c',
    '../src/jalv_config.h',
    '../src/jalv_console.c',
    '../src/jalv_gtk.c',
    '../src/jalv_internal.h',
    '../src/jalv_qt.cpp',
    '../src/jalv_qt.hpp',
    '../src/log.h',
    '../src/lv2_evbuf.h',
    '../src/nodes.h',
    '../src/options.h',
    '../src/port.h',
    '../src/portaudio.c',
    '../src/state.h',
    '../src/symap.h',
    '../src/types.h',
    '../src/urids.h',
    '../src/worker.h',
  )
)

if get_option('lint')
  if not meson.is_subproject()
    # Check release metadata
    autoship = find_program('autoship', required: get_option('tests'))
    if autoship.found()
      test(
        'autoship',
        autoship,
        args: ['test', jalv_src_root],
        suite: 'data',
      )
    endif

    # Check code with cppcheck
    cppcheck = find_program('cppcheck', required: false)
    if cppcheck.found()
      compdb_path = join_paths(jalv_build_root, 'compile_commands.json')
      suppress_path = join_paths(jalv_src_root, '.suppress.cppcheck')
      test(
        'cppcheck',
        cppcheck,
        args: [
          '--enable=warning,style,performance,portability',
          '--error-exitcode=1',
          '--project=' + compdb_path,
          '--suppressions-list=' + suppress_path,
          '-q',
          ['-i', jalv_build_root],
        ],
        suite: 'code',
      )
    endif
  endif

  # Check licensing metadata
  reuse = find_program('reuse', required: false)
  if reuse.found()
    test(
      'REUSE',
      reuse,
      args: ['--root', jalv_src_root, 'lint'],
      suite: 'data',
    )
  endif

  # Check code formatting
  clang_format = find_program('clang-format', required: false)
  if clang_format.found()
    test(
      'format',
      clang_format,
      args: ['--Werror', '--dry-run'] + all_sources,
      suite: 'code',
    )
  endif
endif

##############
# Unit Tests #
##############

test(
  'test_symap',
  executable(
    'test_symap',
    files('../src/symap.c'),
    c_args: c_suppressions + ['-DSYMAP_STANDALONE'],
    dependencies: [zix_dep],
  ),
)
