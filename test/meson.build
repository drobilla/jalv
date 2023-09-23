# Copyright 2019-2022 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

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
endif

##############
# Unit Tests #
##############

test(
  'test_symap',
  executable(
    'test_symap',
    files('../src/symap.c'),
    c_args: ['-DSYMAP_STANDALONE'],
    dependencies: [zix_dep],
  ),
)
