// Copyright 2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_LOG_VIEWER_H
#define JALV_GTK_LOG_VIEWER_H

#include "../log.h"

#include <gtk/gtk.h>

typedef struct {
  GtkWidget*    window;
  GtkListStore* store;
} JalvLogViewer;

void
create_log_viewer_window(JalvLogViewer*  log_viewer,
                         GtkApplication* application,
                         GtkWindow*      parent);

void
log_viewer_append(GtkListStore* store, JalvLogLevel level, const char* message);

#endif // JALV_GTK_LOG_VIEWER_H
