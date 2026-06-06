// Copyright 2007-2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_JALV_GTK_H
#define JALV_GTK_JALV_GTK_H

#include "log_viewer.h"

#include "../types.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/// GUI application state
typedef struct {
  GtkApplication* application;
  const char*     load_arg;
  GtkWindow*      window;
  GtkHeaderBar*   header_bar;
  GMenu*          preset_menu;
  JalvLogViewer   log_viewer;
  GVariant*       remaining;
  unsigned        timer_id;
} App;

/// Widget(s) for a control port or parameter
typedef struct {
  GtkSpinButton* spin;    ///< Spinner for numbers, or null
  GtkWidget*     control; ///< Primary value control
} Controller;

/// Update the UI to reflect the currently loaded preset
void
update_window(Jalv* jalv);

#endif // JALV_GTK_JALV_GTK_H
