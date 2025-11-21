// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "menu.h"

#include "jalv_gtk.h"

#include "../jalv.h"
#include "../state.h"
#include "../types.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <gtk/gtkactionable.h>
#include <lilv/lilv.h>
#include <zix/attributes.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
  GtkMenuItem* item;
  char*        label;
  GtkMenu*     menu;
  GSequence*   banks;
} PresetMenu;

static PresetMenu*
pset_menu_new(char* const label)
{
  PresetMenu* menu = (PresetMenu*)malloc(sizeof(PresetMenu));
  menu->label      = label;
  menu->item       = GTK_MENU_ITEM(gtk_menu_item_new_with_label(menu->label));
  menu->menu       = GTK_MENU(gtk_menu_new());
  menu->banks      = NULL;
  return menu;
}

static void
pset_menu_free(PresetMenu* menu)
{
  if (menu->banks) {
    for (GSequenceIter* i = g_sequence_get_begin_iter(menu->banks);
         !g_sequence_iter_is_end(i);
         i = g_sequence_iter_next(i)) {
      PresetMenu* bank_menu = (PresetMenu*)g_sequence_get(i);
      pset_menu_free(bank_menu);
    }
    g_sequence_free(menu->banks);
  }

  free(menu->label);
  free(menu);
}

static gint
menu_cmp(gconstpointer a, gconstpointer b, gpointer ZIX_UNUSED(data))
{
  return strcmp(((const PresetMenu*)a)->label, ((const PresetMenu*)b)->label);
}

static char*
get_label_string(Jalv* const jalv, const LilvNode* const node)
{
  LilvNode* const label_node =
    lilv_world_get(jalv->world, node, jalv->nodes.rdfs_label, NULL);

  if (!label_node) {
    return g_strdup(lilv_node_as_string(node));
  }

  char* const label = g_strdup(lilv_node_as_string(label_node));
  lilv_node_free(label_node);
  return label;
}

static PresetMenu*
get_bank_menu(Jalv* jalv, PresetMenu* menu, const LilvNode* bank)
{
  char* const    label = get_label_string(jalv, bank);
  PresetMenu     key   = {NULL, label, NULL, NULL};
  GSequenceIter* i     = g_sequence_lookup(menu->banks, &key, menu_cmp, NULL);
  if (!i) {
    PresetMenu* const bank_menu = pset_menu_new(label);
    gtk_menu_item_set_submenu(bank_menu->item, GTK_WIDGET(bank_menu->menu));
    g_sequence_insert_sorted(menu->banks, bank_menu, menu_cmp, NULL);
    return bank_menu;
  }

  g_free(label);
  return (PresetMenu*)g_sequence_get(i);
}

static int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
  PresetMenu* menu  = (PresetMenu*)data;
  const char* label = lilv_node_as_string(title);
  GtkWidget*  item  = gtk_menu_item_new_with_label(label);

  const LilvNode* bank =
    lilv_world_get(jalv->world, node, jalv->nodes.pset_bank, NULL);

  if (bank) {
    PresetMenu* bank_menu = get_bank_menu(jalv, menu, bank);
    gtk_menu_shell_append(GTK_MENU_SHELL(bank_menu->menu), item);
  } else {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu->menu), item);
  }

  gtk_actionable_set_action_name(GTK_ACTIONABLE(item), "win.load-preset");
  gtk_actionable_set_action_target_value(
    GTK_ACTIONABLE(item), g_variant_new("s", lilv_node_as_string(node)));

  return 0;
}

static void
finish_menu(PresetMenu* menu)
{
  for (GSequenceIter* i = g_sequence_get_begin_iter(menu->banks);
       !g_sequence_iter_is_end(i);
       i = g_sequence_iter_next(i)) {
    PresetMenu* bank_menu = (PresetMenu*)g_sequence_get(i);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu->menu),
                          GTK_WIDGET(bank_menu->item));
  }
  g_sequence_free(menu->banks);
}

GtkWidget*
build_menu(Jalv* jalv)
{
  App* const app = (App*)jalv->app;

  GtkWidget* menu_bar  = gtk_menu_bar_new();
  GtkWidget* file      = gtk_menu_item_new_with_mnemonic("_File");
  GtkWidget* file_menu = gtk_menu_new();

  GtkWidget* save = gtk_menu_item_new_with_mnemonic("_Save");
  GtkWidget* quit = gtk_menu_item_new_with_mnemonic("_Quit");

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), file_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file);

  GtkWidget* pset_item   = gtk_menu_item_new_with_mnemonic("_Presets");
  GtkWidget* pset_menu   = gtk_menu_new();
  GtkWidget* save_preset = gtk_menu_item_new_with_mnemonic("_Save Preset...");
  GtkWidget* delete_preset =
    gtk_menu_item_new_with_mnemonic("_Delete Current Preset...");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(pset_item), pset_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu), save_preset);
  gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu), delete_preset);
  gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu),
                        gtk_separator_menu_item_new());
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), pset_item);

  PresetMenu menu = {NULL,
                     NULL,
                     GTK_MENU(pset_menu),
                     g_sequence_new((GDestroyNotify)pset_menu_free)};
  jalv_load_presets(jalv, add_preset_to_menu, &menu);
  finish_menu(&menu);
  app->preset_menu = GTK_MENU(pset_menu);

  gtk_actionable_set_action_name(GTK_ACTIONABLE(quit), "win.quit");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(save), "win.save-as");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(save_preset),
                                 "win.save-preset");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(delete_preset),
                                 "win.delete-preset");

  return menu_bar;
}

void
rebuild_preset_menu(Jalv* jalv)
{
  App* const    app       = (App*)jalv->app;
  GtkContainer* pset_menu = GTK_CONTAINER(app->preset_menu);

  // Clear current menu
  for (GList* items = g_list_nth(gtk_container_get_children(pset_menu), 3);
       items;
       items = items->next) {
    gtk_container_remove(pset_menu, GTK_WIDGET(items->data));
  }

  // Load presets and build new menu
  PresetMenu menu = {NULL,
                     NULL,
                     GTK_MENU(pset_menu),
                     g_sequence_new((GDestroyNotify)pset_menu_free)};
  jalv_load_presets(jalv, add_preset_to_menu, &menu);
  finish_menu(&menu);
  gtk_widget_show_all(GTK_WIDGET(pset_menu));
}
