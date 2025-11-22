// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "jalv_gtk.h"

#include "actions.h"
#include "controls.h"
#include "header.h"
#include "menu.h"

#include "../control.h"
#include "../frontend.h"
#include "../jalv.h"
#include "../log.h"
#include "../options.h"
#include "../query.h"
#include "../types.h"

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <suil/suil.h>
#include <zix/attributes.h>

#include <gdk/gdk.h>
#include <gio/gio.h>
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
setup_options(GApplication* const app, JalvOptions* const opts)
{
  g_set_application_name("Jalv");

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
    {"minimal-ui",
     'm',
     0,
     G_OPTION_ARG_NONE,
     &opts->minimal_ui,
     "Don't show application menu bar or header bar",
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
    {G_OPTION_REMAINING,
     '\0',
     0,
     G_OPTION_ARG_STRING_ARRAY,
     NULL,
     "Exit if the requested JACK client name is taken",
     NULL},
    {0, 0, 0, G_OPTION_ARG_NONE, 0, 0, 0}};

  g_application_add_main_option_entries(app, entries);
  g_application_set_option_context_parameter_string(app, "PLUGIN_STATE");
  g_application_set_option_context_summary(app, "Run an LV2 plugin");
}

int
jalv_frontend_init(Jalv* const jalv)
{
  App* const app = (App*)calloc(1, sizeof(App));

  app->application =
    gtk_application_new("net.drobilla.jalv", G_APPLICATION_NON_UNIQUE);

  jalv->app = app;
  return 0;
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
    if (app->header_bar) {
      gtk_header_bar_set_subtitle(app->header_bar, preset_label);
    }
  } else {
    gtk_window_set_title(app->window, plugin);
  }
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

static void
on_application_startup(GtkApplication* const application, void* const data)
{
  (void)application;

  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  gtk_window_set_default_icon_name("jalv");

  if (!jalv_open(jalv, app->load_arg)) {
    const float update_interval_ms = 1000.0f / jalv->settings.ui_update_hz;

    app->timer_id = g_timeout_add(
      (unsigned)update_interval_ms, (GSourceFunc)jalv_update, jalv);
  }
}

static void
on_application_shutdown(GtkApplication* const application, void* const data)
{
  (void)application;

  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  if (app->timer_id) {
    g_source_remove(app->timer_id);
    app->timer_id = 0U;
  }

  jalv_deactivate(jalv);

  for (unsigned i = 0U; i < jalv->controls.n_controls; ++i) {
    free(jalv->controls.controls[i]->widget); // free Controller
  }

  jalv_close(jalv);
  if (app->remaining) {
    g_variant_unref(app->remaining);
  }
}

static void
on_application_activate(GtkApplication* const application, void* const data)
{
  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  if (!jalv->plugin) {
    // If we made it this far, app should be started and a plugin selected
    g_application_quit(G_APPLICATION(application));
    return;
  }

  GtkWidget* window = gtk_application_window_new(application);
  GtkWidget* vbox   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  app->window = GTK_WINDOW(window);
  update_window_title(jalv);
  gtk_window_set_role(GTK_WINDOW(window), "plugin_ui");
  gtk_container_add(GTK_CONTAINER(window), vbox);

  // Actions

  const GActionEntry app_actions[] = {
    {"about", action_about, NULL, NULL, NULL, {0}},
    {"quit", action_quit, NULL, NULL, NULL, {0}},
  };

  g_action_map_add_action_entries(
    G_ACTION_MAP(application), app_actions, G_N_ELEMENTS(app_actions), jalv);

  const GActionEntry win_actions[] = {
    {"delete-preset", action_delete_preset, NULL, NULL, NULL, {0}},
    {"load-preset", action_load_preset, "s", NULL, NULL, {0}},
    {"save-as", action_save_as, NULL, NULL, NULL, {0}},
    {"save-preset", action_save_preset, NULL, NULL, NULL, {0}},
  };

  g_action_map_add_action_entries(
    G_ACTION_MAP(window), win_actions, G_N_ELEMENTS(win_actions), jalv);

  // Menu bar and/or header bar

  if (!jalv->opts.minimal_ui) {
    GMenu* menu_bar = build_menu_bar(jalv);
    gtk_application_set_menubar(application, G_MENU_MODEL(menu_bar));
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window),
                                            false);

    app->header_bar = build_header_bar(jalv);
    gtk_container_add(GTK_CONTAINER(vbox), GTK_WIDGET(app->header_bar));
  }

  // Accelerators

  gtk_window_add_accel_group(GTK_WINDOW(window), gtk_accel_group_new());

  static const char* action_accels[][2] = {
    {"app.quit", "<Ctrl>Q"},
    {"win.delete-preset", "<Ctrl>Delete"},
    {"win.load-preset", "<Ctrl>L"},
    {"win.save-as", "<Ctrl><Shift>S"},
    {"win.save-preset", "<Ctrl>S"},
    {NULL, NULL}};
  for (unsigned i = 0U; action_accels[i][0]; ++i) {
    const char* accels[] = {action_accels[i][1], NULL};
    gtk_application_set_accels_for_action(
      app->application, action_accels[i][0], accels);
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
    jalv_instantiate_ui(jalv, jalv_frontend_ui_type(), ui_box);
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

    const int controls_width =
      MAX(MAX(box_size.width, controls_size.width) + 24, 640);
    const int controls_height = box_size.height + controls_size.height;

    GdkDisplay* const gdisplay = gdk_display_get_default();
    GdkWindow* const  gwindow  = gtk_widget_get_window(GTK_WIDGET(window));
    GdkMonitor* const monitor =
      gdk_display_get_monitor_at_window(gdisplay, gwindow);

    GdkRectangle     monitor_geometry = {0, 0, 0, 0};
    static const int pad              = 24;
    gdk_monitor_get_workarea(monitor, &monitor_geometry);
    gtk_window_set_default_size(
      GTK_WINDOW(window),
      MIN(monitor_geometry.width - pad, controls_width),
      MIN(monitor_geometry.height - pad, controls_height));
  }

  jalv_activate(jalv);
  jalv_refresh_ui(jalv);
  gtk_window_present(GTK_WINDOW(window));
}

static gint
handle_local_options(GApplication* self, GVariantDict* options, gpointer data)
{
  (void)self;

  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  app->remaining = g_variant_dict_lookup_value(
    options, G_OPTION_REMAINING, G_VARIANT_TYPE_STRING_ARRAY);

  if (app->remaining) {
    size_t              length  = 0U;
    const gchar** const strings = g_variant_get_strv(app->remaining, &length);
    if (length == 1U) {
      app->load_arg = strings[0];
    }

    g_free(strings);

    if (length > 1U) {
      jalv_log(JALV_LOG_ERR, "Unexpected trailing arguments\n");
      return 1;
    }
  }

  return -1;
}

int
jalv_frontend_run(Jalv* jalv)
{
  App* const app = (App*)jalv->app;

  g_signal_connect(G_OBJECT(app->application),
                   "handle-local-options",
                   G_CALLBACK(handle_local_options),
                   jalv);

  g_signal_connect(G_OBJECT(app->application),
                   "startup",
                   G_CALLBACK(on_application_startup),
                   jalv);

  g_signal_connect(G_OBJECT(app->application),
                   "shutdown",
                   G_CALLBACK(on_application_shutdown),
                   jalv);

  g_signal_connect(G_OBJECT(app->application),
                   "activate",
                   G_CALLBACK(on_application_activate),
                   jalv);

  setup_options(G_APPLICATION(app->application), &jalv->opts);

  return g_application_run(
    G_APPLICATION(app->application), jalv->args.argc, jalv->args.argv);
}

int
jalv_frontend_close(Jalv* jalv)
{
  App* const app = (App*)jalv->app;

  g_application_quit(G_APPLICATION(app->application));
  free(jalv->app);
  return 0;
}
