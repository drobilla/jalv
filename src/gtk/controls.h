// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_CONTROLS_H
#define JALV_GTK_CONTROLS_H

#include "../types.h"

#include <gtk/gtk.h>

GtkWidget*
build_control_widget(Jalv* jalv, GtkWidget* window);

#endif // JALV_GTK_CONTROLS_H
