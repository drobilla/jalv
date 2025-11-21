// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_JALV_GTK_H
#define JALV_GTK_JALV_GTK_H

#include "../types.h"

#include <glib.h>
#include <gtk/gtk.h>

/// GUI application state
typedef struct {
  GtkApplication* application;
  const char*     load_arg;
  GtkWindow*      window;
  GtkMenu*        preset_menu;
  GVariant*       remaining;
  unsigned        timer_id;
} App;

/// Widget(s) for a control port or parameter
typedef struct {
  GtkSpinButton* spin;    ///< Spinner for numbers, or null
  GtkWidget*     control; ///< Primary value control
} Controller;

void
update_window_title(Jalv* jalv);

#endif // JALV_GTK_JALV_GTK_H
