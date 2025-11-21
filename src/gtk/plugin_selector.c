// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "../frontend.h"

#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>
#include <lilv/lilv.h>

#include <stddef.h>

static void
on_row_activated(GtkTreeView* const       tree_view,
                 GtkTreePath* const       path,
                 GtkTreeViewColumn* const column,
                 void* const              user_data)
{
  (void)tree_view;
  (void)path;
  (void)column;

  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_ACCEPT);
}

LilvNode*
jalv_frontend_select_plugin(LilvWorld* const world)
{
  // Create the dialog
  GtkWidget* const dialog = gtk_dialog_new_with_buttons(
    "Select Plugin",
    NULL,
    (GtkDialogFlags)(GTK_DIALOG_USE_HEADER_BAR | GTK_DIALOG_MODAL |
                     GTK_DIALOG_DESTROY_WITH_PARENT),
    "_Cancel",
    GTK_RESPONSE_CANCEL,
    "_Load",
    GTK_RESPONSE_ACCEPT,
    NULL);

  gtk_window_set_role(GTK_WINDOW(dialog), "plugin_selector");
  gtk_window_set_default_size(GTK_WINDOW(dialog), 800, 600);

  // Build the treeview

  GtkTreeView* const tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
  {
    // Add treeview to the content area of the dialog
    GtkWidget*    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkContainer* scroll  = GTK_CONTAINER(gtk_scrolled_window_new(NULL, NULL));
    gtk_container_add(scroll, GTK_WIDGET(tree_view));
    gtk_container_add(GTK_CONTAINER(content), GTK_WIDGET(scroll));
    gtk_widget_set_visible(GTK_WIDGET(tree_view), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(scroll), TRUE);
    gtk_box_set_child_packing(
      GTK_BOX(content), GTK_WIDGET(scroll), TRUE, TRUE, 2, GTK_PACK_START);
    gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  }

  // Set treeview columns (all strings)
  static const char* const col_labels[] = {"Name", "Type", "Author", "URI"};
  for (unsigned i = 0; i < 4; ++i) {
    GtkTreeViewColumn* const column = gtk_tree_view_column_new_with_attributes(
      col_labels[i], gtk_cell_renderer_text_new(), "text", i, NULL);

    gtk_tree_view_column_set_sort_column_id(column, i);
    gtk_tree_view_append_column(tree_view, column);
  }

  // Build the model
  GtkListStore* const store = gtk_list_store_new(
    4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
  LILV_FOREACH (plugins, i, plugins) {
    const LilvPlugin* const      p      = lilv_plugins_get(plugins, i);
    LilvNode* const              name   = lilv_plugin_get_name(p);
    const LilvPluginClass* const klass  = lilv_plugin_get_class(p);
    const LilvNode* const        type   = lilv_plugin_class_get_label(klass);
    LilvNode* const              author = lilv_plugin_get_author_name(p);
    const LilvNode* const        uri    = lilv_plugin_get_uri(p);

    if (name && type && uri) {
      GtkTreeIter iter;
      gtk_list_store_insert_with_values(store,
                                        &iter,
                                        -1,
                                        0,
                                        lilv_node_as_string(name),
                                        1,
                                        lilv_node_as_string(type),
                                        2,
                                        lilv_node_as_string(author),
                                        3,
                                        lilv_node_as_string(uri),
                                        -1);
    }

    lilv_node_free(name);
    lilv_node_free(author);
  }

  gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(store));

  g_signal_connect(GTK_WIDGET(tree_view),
                   "row-activated",
                   G_CALLBACK(on_row_activated),
                   dialog);

  // Get URI from selection
  LilvNode* selected_uri = NULL;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    GtkTreeSelection* selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel*     model     = GTK_TREE_MODEL(store);
    GtkTreeIter       selected;
    if (gtk_tree_selection_get_selected(selection, &model, &selected)) {
      GValue val = G_VALUE_INIT;
      gtk_tree_model_get_value(model, &selected, 3, &val);

      selected_uri = lilv_new_uri(world, g_value_get_string(&val));

      g_value_unset(&val);
    }
  }
  gtk_widget_destroy(dialog);

  return selected_uri;
}
