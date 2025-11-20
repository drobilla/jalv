// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_JALV_GTK_H
#define JALV_GTK_JALV_GTK_H

#include <gtk/gtk.h>

/// GUI application state
typedef struct {
  GtkWindow*        window;
  GtkCheckMenuItem* active_preset_item;
} App;

/// Widget(s) for a control port or parameter
typedef struct {
  GtkSpinButton* spin;    ///< Spinner for numbers, or null
  GtkWidget*     control; ///< Primary value control
} Controller;

#endif // JALV_GTK_JALV_GTK_H
