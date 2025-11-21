// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_MENU_H
#define JALV_GTK_MENU_H

#include "../types.h"

#include <gio/gio.h>

GMenu*
build_menu_bar(Jalv* jalv);

void
rebuild_preset_menu(Jalv* jalv);

#endif // JALV_GTK_MENU_H
