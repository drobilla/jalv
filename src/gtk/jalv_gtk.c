// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "jalv_gtk.h"

#include "actions.h"
#include "controls.h"

#include "../control.h"
#include "../frontend.h"
#include "../jalv.h"
#include "../log.h"
#include "../options.h"
#include "../query.h"
#include "../state.h"
#include "../types.h"

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <suil/suil.h>
#include <zix/attributes.h>
#include <zix/sem.h>

#include <gdk/gdk.h>
#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
on_window_destroy(GtkWidget* widget, gpointer data)
{
  (void)widget;
  (void)data;
  gtk_main_quit();
}

int
jalv_frontend_init(ProgramArgs* const args, JalvOptions* const opts)
{
  const GOptionEntry entries[] = {
    {"buffer-size",
     'b',
     0,
     G_OPTION_ARG_INT,
     &opts->ring_size,
     "Buffer size for plugin <=> UI communication",
     "SIZE"},
    {"control",
     'c',
     0,
     G_OPTION_ARG_STRING_ARRAY,
     &opts->controls,
     "Set control value (e.g. \"vol=1.4\")",
     "SETTING"},
    {"dump",
     'd',
     0,
     G_OPTION_ARG_NONE,
     &opts->dump,
     "Dump plugin <=> UI communication",
     NULL},
    {"generic-ui",
     'g',
     0,
     G_OPTION_ARG_NONE,
     &opts->generic_ui,
     "Show generic UI instead of custom plugin GUI",
     NULL},
    {"no-menu",
     'm',
     0,
     G_OPTION_ARG_NONE,
     &opts->no_menu,
     "Do not show Jalv menu on window",
     NULL},
    {"jack-name",
     'n',
     0,
     G_OPTION_ARG_STRING,
     &opts->name,
     "JACK client name",
     "NAME"},
    {"print-controls",
     'p',
     0,
     G_OPTION_ARG_NONE,
     &opts->print_controls,
     "Print control output changes to stdout",
     NULL},
    {"update-frequency",
     'r',
     0,
     G_OPTION_ARG_DOUBLE,
     &opts->update_rate,
     "UI update frequency",
     "HZ"},
    {"scale-factor",
     'S',
     0,
     G_OPTION_ARG_DOUBLE,
     &opts->scale_factor,
     "UI scale factor",
     "FACTOR"},
    {"show-hidden",
     's',
     0,
     G_OPTION_ARG_NONE,
     &opts->show_hidden,
     "Show generic controls for ports marked notOnGUI",
     NULL},
    {"trace",
     't',
     0,
     G_OPTION_ARG_NONE,
     &opts->trace,
     "Print debug trace messages",
     NULL},
    {"ui-uri",
     'U',
     0,
     G_OPTION_ARG_STRING,
     &opts->ui_uri,
     "Load the UI with the given URI",
     "URI"},
    {"exact-jack-name",
     'x',
     0,
     G_OPTION_ARG_NONE,
     &opts->name_exact,
     "Exit if the requested JACK client name is taken",
     NULL},
    {0, 0, 0, G_OPTION_ARG_NONE, 0, 0, 0}};

  GError*   error = NULL;
  const int err =
    gtk_init_with_args(&args->argc,
                       &args->argv,
                       "PLUGIN_STATE - Run an LV2 plugin as a Jack application",
                       entries,
                       NULL,
                       &error);

  if (!err) {
    fprintf(stderr, "%s\n", error->message);
  }

  --args->argc;
  ++args->argv;
  return !err;
}

const char*
jalv_frontend_ui_type(void)
{
#if GTK_MAJOR_VERSION == 3
  return "http://lv2plug.in/ns/extensions/ui#Gtk3UI";
#else
  return NULL;
#endif
}

void
update_window_title(Jalv* jalv)
{
  App* const  app    = (App*)jalv->app;
  const char* plugin = lilv_node_as_string(jalv->plugin_name);

  if (jalv->preset) {
    const char* preset_label = lilv_state_get_label(jalv->preset);
    char*       title        = g_strdup_printf("%s - %s", plugin, preset_label);
    gtk_window_set_title(app->window, title);
    free(title);
  } else {
    gtk_window_set_title(app->window, plugin);
  }
}

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

  g_object_set_data_full(
    G_OBJECT(item), "uri", g_strdup(lilv_node_as_string(node)), g_free);

  g_signal_connect(
    G_OBJECT(item), "activate", G_CALLBACK(on_preset_activate), jalv);

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

static double
get_atom_double(const Jalv* jalv,
                uint32_t    ZIX_UNUSED(size),
                LV2_URID    type,
                const void* body)
{
  if (type == jalv->forge.Int || type == jalv->forge.Bool) {
    return *(const int32_t*)body;
  }

  if (type == jalv->forge.Long) {
    return *(const int64_t*)body;
  }

  if (type == jalv->forge.Float) {
    return *(const float*)body;
  }

  if (type == jalv->forge.Double) {
    return *(const double*)body;
  }

  return NAN;
}

static void
set_combo_box_value(GtkComboBox* const combo_box, const double fvalue)
{
  GtkTreeModel* model = gtk_combo_box_get_model(combo_box);
  GValue        value = G_VALUE_INIT;
  GtkTreeIter   i;
  bool          valid = gtk_tree_model_get_iter_first(model, &i);
  while (valid) {
    gtk_tree_model_get_value(model, &i, 0, &value);
    const double v = g_value_get_float(&value);
    g_value_unset(&value);
    if (fabs(v - fvalue) < FLT_EPSILON) {
      gtk_combo_box_set_active_iter(combo_box, &i);
      return;
    }
    valid = gtk_tree_model_iter_next(model, &i);
  }
}

void
jalv_frontend_set_control(const Jalv* const    jalv,
                          const Control* const control,
                          const uint32_t       value_size,
                          const uint32_t       value_type,
                          const void* const    value_body)
{
  Controller* const controller = (Controller*)control->widget;
  if (!controller) {
    return;
  }

  GtkWidget* const widget = controller->control;
  if (value_type == jalv->urids.atom_String && GTK_IS_ENTRY(widget)) {
    gtk_entry_set_text(GTK_ENTRY(widget), (const char*)value_body);
    return;
  }

  if (value_type == jalv->urids.atom_Path && GTK_IS_FILE_CHOOSER(widget)) {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(widget),
                                  (const char*)value_body);
    return;
  }

  const double fvalue =
    get_atom_double(jalv, value_size, value_type, value_body);
  if (isnan(fvalue)) {
    jalv_log(JALV_LOG_WARNING, "Expected numeric control value\n");
  } else if (GTK_IS_COMBO_BOX(widget)) {
    set_combo_box_value(GTK_COMBO_BOX(widget), fvalue);
  } else if (GTK_IS_TOGGLE_BUTTON(widget)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), fvalue > 0.0f);
  } else if (GTK_IS_SWITCH(widget)) {
    gtk_switch_set_active(GTK_SWITCH(widget), fvalue > 0.f);
  } else if (GTK_IS_RANGE(widget)) {
    gtk_range_set_value(GTK_RANGE(widget), fvalue);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(controller->spin), fvalue);
  } else {
    jalv_log(JALV_LOG_WARNING, "Unknown widget type for value\n");
  }
}

static LV2UI_Request_Value_Status
on_request_value(LV2UI_Feature_Handle      handle,
                 const LV2_URID            key,
                 const LV2_URID            ZIX_UNUSED(type),
                 const LV2_Feature* const* ZIX_UNUSED(features))
{
  Jalv* const    jalv    = (Jalv*)handle;
  App* const     app     = (App*)jalv->app;
  const Control* control = get_property_control(&jalv->controls, key);

  if (!control) {
    return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;
  }

  if (control->value_type != jalv->forge.Path) {
    return LV2UI_REQUEST_VALUE_ERR_UNSUPPORTED;
  }

  if (jalv->updating) {
    return LV2UI_REQUEST_VALUE_BUSY;
  }

  GtkWidget* dialog = gtk_file_chooser_dialog_new("Choose file",
                                                  app->window,
                                                  GTK_FILE_CHOOSER_ACTION_OPEN,
                                                  "_Cancel",
                                                  GTK_RESPONSE_CANCEL,
                                                  "_OK",
                                                  GTK_RESPONSE_OK,
                                                  NULL);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
    char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    jalv_set_control(jalv, control, strlen(path) + 1, jalv->forge.Path, path);
    g_free(path);
  }

  gtk_widget_destroy(dialog);

  return 0;
}

static void
build_menu(Jalv* jalv, GtkWidget* window, GtkWidget* vbox)
{
  App* const app = (App*)jalv->app;

  GtkWidget* menu_bar  = gtk_menu_bar_new();
  GtkWidget* file      = gtk_menu_item_new_with_mnemonic("_File");
  GtkWidget* file_menu = gtk_menu_new();

  GtkAccelGroup* ag = gtk_accel_group_new();
  gtk_window_add_accel_group(GTK_WINDOW(window), ag);

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

  g_signal_connect(
    G_OBJECT(quit), "activate", G_CALLBACK(on_quit_activate), jalv);

  g_signal_connect(
    G_OBJECT(save), "activate", G_CALLBACK(on_save_activate), jalv);

  g_signal_connect(G_OBJECT(save_preset),
                   "activate",
                   G_CALLBACK(on_save_preset_activate),
                   jalv);

  g_signal_connect(G_OBJECT(delete_preset),
                   "activate",
                   G_CALLBACK(on_delete_preset_activate),
                   jalv);

  gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);
}

bool
jalv_frontend_discover(const Jalv* ZIX_UNUSED(jalv))
{
  return TRUE;
}

float
jalv_frontend_refresh_rate(const Jalv* ZIX_UNUSED(jalv))
{
  GdkDisplay* const display = gdk_display_get_default();
  GdkMonitor* const monitor = gdk_display_get_primary_monitor(display);

  const float rate = (float)gdk_monitor_get_refresh_rate(monitor);

  return rate < 30.0f ? 30.0f : rate;
}

float
jalv_frontend_scale_factor(const Jalv* ZIX_UNUSED(jalv))
{
  GdkDisplay* const display = gdk_display_get_default();
  GdkMonitor* const monitor = gdk_display_get_primary_monitor(display);

  return (float)gdk_monitor_get_scale_factor(monitor);
}

int
jalv_frontend_open(Jalv* jalv, const ProgramArgs ZIX_UNUSED(args))
{
  // Create top-level app, window and vbox
  App* const app    = (App*)calloc(1, sizeof(App));
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget* vbox   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  app->window = GTK_WINDOW(window);
  jalv->app   = app;

  update_window_title(jalv);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), jalv);
  gtk_window_set_role(GTK_WINDOW(window), "plugin_ui");
  gtk_container_add(GTK_CONTAINER(window), vbox);

  if (!jalv->opts.no_menu) {
    build_menu(jalv, window, vbox);
  }

  // Create and show a box to contain the plugin UI
  GtkWidget* ui_box = gtk_event_box_new();
  gtk_widget_set_halign(ui_box, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(ui_box, TRUE);
  gtk_widget_set_valign(ui_box, GTK_ALIGN_FILL);
  gtk_widget_set_vexpand(ui_box, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), ui_box, TRUE, TRUE, 0);
  gtk_widget_show(ui_box);
  gtk_widget_show(vbox);

  // Attempt to instantiate custom UI if necessary
  if (jalv->ui && !jalv->opts.generic_ui) {
    jalv_ui_instantiate(jalv, jalv_frontend_ui_type(), ui_box);
  }

  jalv->features.request_value.request = on_request_value;

  if (jalv->ui_instance) {
    GtkWidget* widget = (GtkWidget*)suil_instance_get_widget(jalv->ui_instance);

    gtk_container_add(GTK_CONTAINER(ui_box), widget);
    gtk_window_set_resizable(GTK_WINDOW(window),
                             jalv_ui_is_resizable(jalv->world, jalv->ui));
    gtk_widget_show_all(vbox);
    gtk_widget_grab_focus(widget);
  } else {
    GtkWidget* controls   = build_control_widget(jalv, window);
    GtkWidget* scroll_win = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll_win), controls);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_win),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ui_box), scroll_win);
    gtk_widget_set_margin_top(controls, 8);
    gtk_widget_set_margin_bottom(controls, 8);
    gtk_widget_show_all(vbox);

    GtkRequisition controls_size = {0, 0};
    GtkRequisition box_size      = {0, 0};
    gtk_widget_get_preferred_size(GTK_WIDGET(controls), NULL, &controls_size);
    gtk_widget_get_preferred_size(GTK_WIDGET(vbox), NULL, &box_size);

    gtk_window_set_default_size(
      GTK_WINDOW(window),
      MAX(MAX(box_size.width, controls_size.width) + 24, 640),
      box_size.height + controls_size.height);
  }

  jalv_init_ui(jalv);

  const float update_interval_ms = 1000.0f / jalv->settings.ui_update_hz;
  g_timeout_add((unsigned)update_interval_ms, (GSourceFunc)jalv_update, jalv);

  gtk_window_present(GTK_WINDOW(window));
  gtk_main();

  suil_instance_free(jalv->ui_instance);

  for (unsigned i = 0U; i < jalv->controls.n_controls; ++i) {
    free(jalv->controls.controls[i]->widget); // free Controller
  }

  jalv->ui_instance = NULL;
  zix_sem_post(&jalv->done);
  return 0;
}

int
jalv_frontend_close(Jalv* jalv)
{
  gtk_main_quit();
  free(jalv->app);
  return 0;
}
