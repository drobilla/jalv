# Copyright 2024-2025 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

icon_sizes = [
  '16x16',
  '24x24',
  '32x32',
  '48x48',
  '64x64',
  '128x128',
  '256x256',
]

icons_dir = get_option('prefix') / get_option('datadir') / 'icons' / 'hicolor'

install_data(
  'scalable/jalv.svg',
  install_dir: icons_dir / 'scalable' / 'apps',
)

foreach size : icon_sizes
  install_data(
    files(size / 'jalv.png'),
    install_dir: icons_dir / size / 'apps',
  )
endforeach
