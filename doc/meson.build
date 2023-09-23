# Copyright 2022-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: 0BSD OR ISC

install_man(files('jalv.1'))

if not get_option('gtk3').disabled()
  install_man(files('jalv.gtk3.1'))
endif

if not get_option('qt5').disabled()
  install_man(files('jalv.qt5.1'))
endif
