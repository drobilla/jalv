// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "menu.h"

#include "jalv_gtk.h"

#include "../jalv.h"
#include "../state.h"
#include "../types.h"

#include <glib.h>
#include <lilv/lilv.h>
#include <zix/attributes.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
  GMenuItem* item;
  char*      label;
  GMenu*     menu;
  GSequence* banks;
} PresetMenu;

typedef struct {
  GMenu*     menu;
  GMenuItem* item;
} Submenu;

static Submenu
submenu_new(GMenu* const menu, const char* const label)
{
  const Submenu submenu = {menu, g_menu_item_new(label, NULL)};
  g_menu_item_set_submenu(submenu.item, G_MENU_MODEL(submenu.menu));
  return submenu;
}

static Submenu
subsection_new(GMenu* const menu)
{
  const Submenu subsection = {
    menu, g_menu_item_new_section(NULL, G_MENU_MODEL(menu))};
  return subsection;
}

static PresetMenu*
pset_menu_new(char* const label)
{
  PresetMenu* menu = (PresetMenu*)malloc(sizeof(PresetMenu));
  menu->label      = label;
  menu->item       = G_MENU_ITEM(g_menu_item_new(menu->label, NULL));
  menu->menu       = G_MENU(g_menu_new());
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
    g_menu_item_set_submenu(bank_menu->item, G_MENU_MODEL(bank_menu->menu));
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
  const char* uri   = lilv_node_as_string(node);
  const char* label = lilv_node_as_string(title);

  char* const detailed_action = g_strdup_printf("win.load-preset(\"%s\")", uri);
  GMenuItem*  item            = g_menu_item_new(label, detailed_action);

  const LilvNode* bank =
    lilv_world_get(jalv->world, node, jalv->nodes.pset_bank, NULL);

  if (bank) {
    PresetMenu* bank_menu = get_bank_menu(jalv, menu, bank);
    g_menu_append_item(bank_menu->menu, item);
  } else {
    g_menu_append_item(menu->menu, item);
  }

  g_free(detailed_action);
  return 0;
}

static void
finish_menu(PresetMenu* menu)
{
  for (GSequenceIter* i = g_sequence_get_begin_iter(menu->banks);
       !g_sequence_iter_is_end(i);
       i = g_sequence_iter_next(i)) {
    PresetMenu* bank_menu = (PresetMenu*)g_sequence_get(i);
    g_menu_append_item(menu->menu, bank_menu->item);
  }
  g_sequence_free(menu->banks);
}

static void
append_preset_operation_items(GMenu* const menu)
{
  GMenuItem* const save_preset =
    g_menu_item_new("_Save Preset...", "win.save-preset");
  GMenuItem* const delete_preset =
    g_menu_item_new("_Delete Preset...", "win.delete-preset");

  g_menu_append_item(menu, save_preset);
  g_menu_append_item(menu, delete_preset);
}

static GMenu*
build_load_preset_menu(Jalv* const jalv)
{
  GMenu* const     menu  = g_menu_new();
  GSequence* const banks = g_sequence_new((GDestroyNotify)pset_menu_free);
  PresetMenu       data  = {NULL, NULL, menu, banks};

  jalv_load_presets(jalv, add_preset_to_menu, &data);
  finish_menu(&data);
  return menu;
}

GMenu*
build_menu_bar(Jalv* jalv)
{
  App* const   app      = (App*)jalv->app;
  GMenu* const menu_bar = g_menu_new();

  // File
  Submenu    file_menu = submenu_new(g_menu_new(), "_File");
  GMenuItem* save_as   = g_menu_item_new("_Save As...", "win.save-as");
  GMenuItem* quit      = g_menu_item_new("_Quit", "app.quit");
  g_menu_append_item(file_menu.menu, save_as);
  g_menu_append_item(file_menu.menu, quit);
  g_menu_append_item(menu_bar, file_menu.item);

  // Presets
  Submenu pset_menu  = submenu_new(g_menu_new(), "_Presets");
  Submenu op_section = subsection_new(g_menu_new());
  append_preset_operation_items(op_section.menu);

  // Presets -> Load Preset
  Submenu load_menu = submenu_new(build_load_preset_menu(jalv), "_Load Preset");
  app->preset_menu  = load_menu.menu; // Keep a pointer for later rebuilding

  g_menu_append_item(pset_menu.menu, op_section.item);
  g_menu_append_item(pset_menu.menu, load_menu.item);
  g_menu_append_item(menu_bar, pset_menu.item);
  return menu_bar;
}

void
rebuild_preset_menu(Jalv* jalv)
{
  App* const   app       = (App*)jalv->app;
  GMenu* const pset_menu = app->preset_menu;

  // Clear current menu
  g_menu_remove_all(pset_menu);

  // Load presets and build new menu
  PresetMenu menu = {
    NULL, NULL, pset_menu, g_sequence_new((GDestroyNotify)pset_menu_free)};
  jalv_load_presets(jalv, add_preset_to_menu, &menu);
  finish_menu(&menu);
}
