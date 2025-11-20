// Copyright 2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_APP_H
#define JALV_GTK_APP_H

#include <gtk/gtk.h>

typedef struct {
  GtkWindow*        window;
  GtkCheckMenuItem* active_preset_item;
} App;

#endif // JALV_GTK_APP_H
