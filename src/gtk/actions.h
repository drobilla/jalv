// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_ACTIONS_H
#define JALV_GTK_ACTIONS_H

#include <glib.h>
#include <gtk/gtk.h>

void
on_delete_preset_activate(GtkWidget* widget, gpointer data);

void
on_preset_activate(GtkWidget* widget, gpointer data);

void
on_quit_activate(GtkWidget* widget, gpointer data);

void
on_save_activate(GtkWidget* widget, gpointer data);

void
on_save_preset_activate(GtkWidget* widget, gpointer data);

#endif // JALV_GTK_ACTIONS_H
