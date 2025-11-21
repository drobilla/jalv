// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_MENU_H
#define JALV_GTK_MENU_H

#include "../types.h"

#include <gtk/gtk.h>

GtkWidget*
build_menu(Jalv* jalv);

void
rebuild_preset_menu(Jalv* jalv);

#endif // JALV_GTK_MENU_H
