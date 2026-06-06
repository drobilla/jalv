// Copyright 2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "log_viewer.h"

#include "../log.h"

#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>

#include <stddef.h>

static void
on_clear(GtkButton* const self, void* const user_data)
{
  (void)self;

  GtkListStore* const store = (GtkListStore*)user_data;
  gtk_list_store_clear(store);
}

static void
on_destroy(GtkWidget* const self, void* const user_data)
{
  (void)self;

  JalvLogViewer* const log_viewer = (JalvLogViewer*)user_data;

  log_viewer->window = NULL;
}

void
log_viewer_append(GtkListStore* const store,
                  const JalvLogLevel  level,
                  const char* const   message)
{
  static const char* const log_level_strings[] = {
    NULL, NULL, NULL, "error", "warning", NULL, "info", "debug"};

  GTimeZone* const timezone = g_time_zone_new_local();
  GDateTime* const datetime = g_date_time_new_now(timezone);

  char* const iso_time_string = g_date_time_format_iso8601(datetime);
  char* const time_string     = g_date_time_format(datetime, "%b %d %H:%M:%S");

  const char* const foreground_string = (level == JALV_LOG_ERR)       ? "Red"
                                        : (level == JALV_LOG_WARNING) ? "Orange"
                                                                      : NULL;

  GtkTreeIter iter;
  gtk_list_store_insert_with_values(store,
                                    &iter,
                                    -1,
                                    0,
                                    iso_time_string,
                                    1,
                                    foreground_string,
                                    2,
                                    time_string,
                                    3,
                                    log_level_strings[level],
                                    4,
                                    message,
                                    -1);
}

void
create_log_viewer_window(JalvLogViewer*        log_viewer,
                         GtkApplication* const application,
                         GtkWindow* const      parent)
{
  GtkWidget* const window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

  gtk_window_set_application(GTK_WINDOW(window), application);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  gtk_window_set_role(GTK_WINDOW(window), "log_viewer");
  gtk_window_set_title(GTK_WINDOW(window), "Log");
  gtk_window_set_transient_for(GTK_WINDOW(window), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);

  // Header bar

  GtkHeaderBar* const header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
  gtk_header_bar_set_title(header_bar, "Log");
  gtk_header_bar_set_show_close_button(header_bar, TRUE);
  gtk_header_bar_set_decoration_layout(header_bar, ":close");

  GtkWidget* const clear_button = gtk_button_new_from_icon_name(
    "edit-clear-history", GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_button_set_relief(GTK_BUTTON(clear_button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(clear_button, "Clear all log entries");

  GtkToolItem* const clear_button_item = gtk_tool_item_new();
  gtk_container_add(GTK_CONTAINER(clear_button_item), clear_button);

  gtk_widget_show(GTK_WIDGET(clear_button_item));
  gtk_widget_show(GTK_WIDGET(clear_button));
  gtk_widget_show_all(GTK_WIDGET(header_bar));
  gtk_header_bar_pack_end(header_bar, GTK_WIDGET(clear_button_item));
  gtk_window_set_titlebar(GTK_WINDOW(window), GTK_WIDGET(header_bar));

  // List view

  GtkTreeView* const tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
  {
    // Add treeview to the content area of the dialog
    GtkContainer* scroll = GTK_CONTAINER(gtk_scrolled_window_new(NULL, NULL));
    gtk_container_add(scroll, GTK_WIDGET(tree_view));
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(scroll));
    gtk_widget_set_visible(GTK_WIDGET(tree_view), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(scroll), TRUE);
    gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  }

  GtkTreeViewColumn* const time_col = gtk_tree_view_column_new_with_attributes(
    "Time", gtk_cell_renderer_text_new(), "text", 2, NULL);
  GtkTreeViewColumn* const level_col = gtk_tree_view_column_new_with_attributes(
    "Level", gtk_cell_renderer_text_new(), "text", 3, "foreground", 1, NULL);
  GtkTreeViewColumn* const message_col =
    gtk_tree_view_column_new_with_attributes(
      "Message", gtk_cell_renderer_text_new(), "text", 4, NULL);

  gtk_tree_view_column_set_sort_column_id(time_col, 0);
  gtk_tree_view_append_column(tree_view, time_col);
  gtk_tree_view_column_set_sort_column_id(level_col, 3);
  gtk_tree_view_append_column(tree_view, level_col);
  gtk_tree_view_column_set_sort_column_id(message_col, 4);
  gtk_tree_view_append_column(tree_view, message_col);

  g_signal_connect(
    clear_button, "clicked", G_CALLBACK(on_clear), log_viewer->store);
  g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), log_viewer);

  gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(log_viewer->store));

  log_viewer->window = window;
}
