// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "control.h"
#include "frontend.h"
#include "jalv.h"
#include "log.h"
#include "options.h"
#include "query.h"
#include "state.h"
#include "types.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
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

/* TODO: Gtk only provides one pointer for value changed callbacks, which we
   use for the Control.  So, there is no easy way to pass both a Control
   and Jalv which is needed to actually do anything with a control.  Work
   around this by statically storing the Jalv instance.  Since this UI isn't a
   module, this isn't the end of the world, but a global "god pointer" is a
   pretty bad smell.  Refactoring things to be more modular or changing how Gtk
   signals are connected would be a good idea.
*/
static Jalv* s_jalv = NULL;

static GtkCheckMenuItem* active_preset_item = NULL;
static bool              updating           = false;

/// Widget(s) for a control port or parameter
typedef struct {
  GtkSpinButton* spin;    ///< Spinner for numbers, or null
  GtkWidget*     control; ///< Primary value control
} Controller;

static void
on_window_destroy(GtkWidget* widget, gpointer data)
{
  (void)widget;
  (void)data;
  gtk_main_quit();
}

int
jalv_frontend_init(JalvFrontendArgs* const args, JalvOptions* const opts)
{
  const GOptionEntry entries[] = {
    {"preset",
     'P',
     0,
     G_OPTION_ARG_STRING,
     &opts->preset,
     "Load state from preset",
     "URI"},
    {"scale-factor",
     'S',
     0,
     G_OPTION_ARG_DOUBLE,
     &opts->scale_factor,
     "UI scale factor",
     "SCALE"},
    {"ui-uri",
     'U',
     0,
     G_OPTION_ARG_STRING,
     &opts->ui_uri,
     "Load the UI with the given URI",
     "URI"},
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
     "Use Jalv generic UI and not the plugin UI",
     NULL},
    {"load",
     'l',
     0,
     G_OPTION_ARG_STRING,
     &opts->load,
     "Load state from save directory",
     "DIR"},
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
    {"show-hidden",
     's',
     0,
     G_OPTION_ARG_NONE,
     &opts->show_hidden,
     "Show controls for ports with notOnGUI property on generic UI",
     NULL},
    {"trace",
     't',
     0,
     G_OPTION_ARG_NONE,
     &opts->trace,
     "Print debug trace messages",
     NULL},
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
    gtk_init_with_args(args->argc,
                       args->argv,
                       "PLUGIN_URI - Run an LV2 plugin as a Jack application",
                       entries,
                       NULL,
                       &error);

  if (!err) {
    fprintf(stderr, "%s\n", error->message);
  }

  --*args->argc;
  ++*args->argv;
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

static void
on_save_activate(GtkWidget* ZIX_UNUSED(widget), void* ptr)
{
  Jalv*      jalv = (Jalv*)ptr;
  GtkWidget* dialog =
    gtk_file_chooser_dialog_new("Save State",
                                (GtkWindow*)jalv->window,
                                GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER,
                                "_Cancel",
                                GTK_RESPONSE_CANCEL,
                                "_Save",
                                GTK_RESPONSE_ACCEPT,
                                NULL);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    char* base = g_build_filename(path, "/", NULL);
    jalv_save(jalv, base);
    g_free(path);
    g_free(base);
  }

  gtk_widget_destroy(dialog);
}

static void
on_quit_activate(GtkWidget* ZIX_UNUSED(widget), gpointer data)
{
  GtkWidget* window = (GtkWidget*)data;
  gtk_widget_destroy(window);
}

typedef struct {
  Jalv*     jalv;
  LilvNode* preset;
} PresetRecord;

static char*
symbolify(const char* in)
{
  const size_t len = strlen(in);
  char*        out = (char*)calloc(len + 1, 1);
  for (size_t i = 0; i < len; ++i) {
    if (g_ascii_isalnum(in[i])) {
      out[i] = in[i];
    } else {
      out[i] = '_';
    }
  }
  return out;
}

static void
set_window_title(Jalv* jalv)
{
  LilvNode*   name   = lilv_plugin_get_name(jalv->plugin);
  const char* plugin = lilv_node_as_string(name);
  if (jalv->preset) {
    const char* preset_label = lilv_state_get_label(jalv->preset);
    char*       title        = g_strdup_printf("%s - %s", plugin, preset_label);
    gtk_window_set_title(GTK_WINDOW(jalv->window), title);
    free(title);
  } else {
    gtk_window_set_title(GTK_WINDOW(jalv->window), plugin);
  }
  lilv_node_free(name);
}

static void
on_preset_activate(GtkWidget* widget, gpointer data)
{
  if (GTK_CHECK_MENU_ITEM(widget) != active_preset_item) {
    PresetRecord* record = (PresetRecord*)data;
    jalv_apply_preset(record->jalv, record->preset);
    if (active_preset_item) {
      gtk_check_menu_item_set_active(active_preset_item, FALSE);
    }

    active_preset_item = GTK_CHECK_MENU_ITEM(widget);
    gtk_check_menu_item_set_active(active_preset_item, TRUE);
    set_window_title(record->jalv);
  }
}

static void
on_preset_destroy(gpointer data, GClosure* ZIX_UNUSED(closure))
{
  PresetRecord* record = (PresetRecord*)data;
  lilv_node_free(record->preset);
  free(record);
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
  GtkWidget*  item  = gtk_check_menu_item_new_with_label(label);
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
  if (jalv->preset &&
      lilv_node_equals(lilv_state_get_uri(jalv->preset), node)) {
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
    active_preset_item = GTK_CHECK_MENU_ITEM(item);
  }

  const LilvNode* bank =
    lilv_world_get(jalv->world, node, jalv->nodes.pset_bank, NULL);

  if (bank) {
    PresetMenu* bank_menu = get_bank_menu(jalv, menu, bank);
    gtk_menu_shell_append(GTK_MENU_SHELL(bank_menu->menu), item);
  } else {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu->menu), item);
  }

  PresetRecord* record = (PresetRecord*)malloc(sizeof(PresetRecord));
  record->jalv         = jalv;
  record->preset       = lilv_node_duplicate(node);

  g_signal_connect_data(G_OBJECT(item),
                        "activate",
                        G_CALLBACK(on_preset_activate),
                        record,
                        on_preset_destroy,
                        (GConnectFlags)0);

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

static void
rebuild_preset_menu(Jalv* jalv, GtkContainer* pset_menu)
{
  // Clear current menu
  active_preset_item = NULL;
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

static void
on_save_preset_activate(GtkWidget* widget, void* ptr)
{
  Jalv* jalv = (Jalv*)ptr;

  GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Preset",
                                                  (GtkWindow*)jalv->window,
                                                  GTK_FILE_CHOOSER_ACTION_SAVE,
                                                  "_Cancel",
                                                  GTK_RESPONSE_REJECT,
                                                  "_Save",
                                                  GTK_RESPONSE_ACCEPT,
                                                  NULL);

  char* dot_lv2 = g_build_filename(g_get_home_dir(), ".lv2", NULL);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dot_lv2);
  free(dot_lv2);

  GtkWidget* content   = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkBox*    box       = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
  GtkWidget* uri_label = gtk_label_new("URI (Optional):");
  GtkWidget* uri_entry = gtk_entry_new();
  GtkWidget* add_prefix =
    gtk_check_button_new_with_mnemonic("_Prefix plugin name");

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(add_prefix), TRUE);
  gtk_box_pack_start(box, uri_label, FALSE, TRUE, 2);
  gtk_box_pack_start(box, uri_entry, TRUE, TRUE, 2);
  gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(box), FALSE, FALSE, 6);
  gtk_box_pack_start(GTK_BOX(content), add_prefix, FALSE, FALSE, 6);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  gtk_entry_set_activates_default(GTK_ENTRY(uri_entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    LilvNode*   plug_name = lilv_plugin_get_name(jalv->plugin);
    const char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    const char* uri  = gtk_entry_get_text(GTK_ENTRY(uri_entry));
    const char* prefix = "";
    const char* sep    = "";
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_prefix))) {
      prefix = lilv_node_as_string(plug_name);
      sep    = "_";
    }

    char* dirname  = g_path_get_dirname(path);
    char* basename = g_path_get_basename(path);
    char* sym      = symbolify(basename);
    char* sprefix  = symbolify(prefix);
    char* bundle   = g_strjoin(NULL, sprefix, sep, sym, ".preset.lv2/", NULL);
    char* file     = g_strjoin(NULL, sym, ".ttl", NULL);
    char* dir      = g_build_filename(dirname, bundle, NULL);

    jalv_save_preset(jalv, dir, (strlen(uri) ? uri : NULL), basename, file);

    // Reload bundle into the world
    LilvNode* ldir = lilv_new_file_uri(jalv->world, NULL, dir);
    lilv_world_unload_bundle(jalv->world, ldir);
    lilv_world_load_bundle(jalv->world, ldir);
    lilv_node_free(ldir);

    // Rebuild preset menu and update window title
    rebuild_preset_menu(jalv, GTK_CONTAINER(gtk_widget_get_parent(widget)));
    set_window_title(jalv);

    g_free(dir);
    g_free(file);
    g_free(bundle);
    free(sprefix);
    free(sym);
    g_free(basename);
    g_free(dirname);
    lilv_node_free(plug_name);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_delete_preset_activate(GtkWidget* widget, void* ptr)
{
  Jalv* jalv = (Jalv*)ptr;
  if (!jalv->preset) {
    return;
  }

  GtkWidget* dialog = gtk_dialog_new_with_buttons(
    "Delete Preset?",
    (GtkWindow*)jalv->window,
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
    "_Cancel",
    GTK_RESPONSE_CANCEL,
    "_OK",
    GTK_RESPONSE_ACCEPT,
    NULL);

  char* msg = g_strdup_printf("Delete preset \"%s\" from the file system?",
                              lilv_state_get_label(jalv->preset));

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* text    = gtk_label_new(msg);
  gtk_box_pack_start(GTK_BOX(content), text, TRUE, TRUE, 4);

  gtk_widget_show_all(dialog);
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    jalv_delete_current_preset(jalv);
    rebuild_preset_menu(jalv, GTK_CONTAINER(gtk_widget_get_parent(widget)));
  }

  lilv_state_free(jalv->preset);
  jalv->preset = NULL;
  set_window_title(jalv);

  g_free(msg);
  gtk_widget_destroy(text);
  gtk_widget_destroy(dialog);
}

static void
set_control(const Control* control,
            uint32_t       size,
            LV2_URID       type,
            const void*    body)
{
  if (!updating) {
    jalv_set_control(s_jalv, control, size, type, body);
  }
}

static bool
differ_enough(float a, float b)
{
  return fabsf(a - b) >= FLT_EPSILON;
}

static void
set_float_control(const Control* control, float value)
{
  const LV2_Atom_Forge* const forge = &s_jalv->forge;
  if (control->value_type == forge->Int) {
    const int32_t ival = lrintf(value);
    set_control(control, sizeof(ival), forge->Int, &ival);
  } else if (control->value_type == forge->Long) {
    const int64_t lval = lrintf(value);
    set_control(control, sizeof(lval), forge->Long, &lval);
  } else if (control->value_type == forge->Float) {
    set_control(control, sizeof(value), forge->Float, &value);
  } else if (control->value_type == forge->Double) {
    const double dval = value;
    set_control(control, sizeof(dval), forge->Double, &dval);
  } else if (control->value_type == forge->Bool) {
    const int32_t ival = value;
    set_control(control, sizeof(ival), forge->Bool, &ival);
  }

  Controller* controller = (Controller*)control->widget;
  if (controller && controller->spin &&
      differ_enough(gtk_spin_button_get_value(controller->spin), value)) {
    gtk_spin_button_set_value(controller->spin, value);
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

static void
control_changed(const Jalv* jalv,
                Controller* controller,
                uint32_t    size,
                LV2_URID    type,
                const void* body)
{
  GtkWidget* const widget = controller->control;

  if (GTK_IS_ENTRY(widget) && type == jalv->urids.atom_String) {
    gtk_entry_set_text(GTK_ENTRY(widget), (const char*)body);
    return;
  }

  if (GTK_IS_FILE_CHOOSER(widget) && type == jalv->urids.atom_Path) {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(widget), (const char*)body);
    return;
  }

  const double fvalue = get_atom_double(jalv, size, type, body);
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

static int
patch_set_get(Jalv*                  jalv,
              const LV2_Atom_Object* obj,
              const LV2_Atom_URID**  property,
              const LV2_Atom**       value)
{
  lv2_atom_object_get(obj,
                      jalv->urids.patch_property,
                      (const LV2_Atom*)property,
                      jalv->urids.patch_value,
                      value,
                      0);
  if (!*property) {
    jalv_log(JALV_LOG_WARNING, "patch:Set message with no property\n");
    return 1;
  }

  if ((*property)->atom.type != jalv->forge.URID) {
    jalv_log(JALV_LOG_WARNING, "patch:Set property is not a URID\n");
    return 1;
  }

  return 0;
}

static int
patch_put_get(Jalv*                   jalv,
              const LV2_Atom_Object*  obj,
              const LV2_Atom_Object** body)
{
  lv2_atom_object_get(obj, jalv->urids.patch_body, (const LV2_Atom*)body, 0);
  if (!*body) {
    jalv_log(JALV_LOG_WARNING, "patch:Put message with no body\n");
    return 1;
  }

  if (!lv2_atom_forge_is_object_type(&jalv->forge, (*body)->atom.type)) {
    jalv_log(JALV_LOG_WARNING, "patch:Put body is not an object\n");
    return 1;
  }

  return 0;
}

static LV2UI_Request_Value_Status
on_request_value(LV2UI_Feature_Handle      handle,
                 const LV2_URID            key,
                 const LV2_URID            ZIX_UNUSED(type),
                 const LV2_Feature* const* ZIX_UNUSED(features))
{
  Jalv* const    jalv    = (Jalv*)handle;
  const Control* control = get_property_control(&jalv->controls, key);

  if (!control) {
    return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;
  }

  if (control->value_type != jalv->forge.Path) {
    return LV2UI_REQUEST_VALUE_ERR_UNSUPPORTED;
  }

  GtkWidget* dialog = gtk_file_chooser_dialog_new("Choose file",
                                                  GTK_WINDOW(jalv->window),
                                                  GTK_FILE_CHOOSER_ACTION_OPEN,
                                                  "_Cancel",
                                                  GTK_RESPONSE_CANCEL,
                                                  "_OK",
                                                  GTK_RESPONSE_OK,
                                                  NULL);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
    char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

    set_control(control, strlen(path) + 1, jalv->forge.Path, path);

    g_free(path);
  }

  gtk_widget_destroy(dialog);

  return 0;
}

static void
property_changed(Jalv* jalv, LV2_URID key, const LV2_Atom* value)
{
  Control* control = get_property_control(&jalv->controls, key);
  if (control) {
    control_changed(
      jalv, (Controller*)control->widget, value->size, value->type, value + 1);
  }
}

void
jalv_frontend_port_event(Jalv*       jalv,
                         uint32_t    port_index,
                         uint32_t    buffer_size,
                         uint32_t    protocol,
                         const void* buffer)
{
  if (jalv->ui_instance) {
    suil_instance_port_event(
      jalv->ui_instance, port_index, buffer_size, protocol, buffer);
    return;
  }

  if (protocol == 0) {
    Controller* const controller = (Controller*)jalv->ports[port_index].widget;
    if (controller) {
      control_changed(jalv, controller, buffer_size, jalv->forge.Float, buffer);
    }

    return;
  }

  if (protocol != jalv->urids.atom_eventTransfer) {
    jalv_log(JALV_LOG_WARNING, "Unknown port event protocol\n");
    return;
  }

  const LV2_Atom* atom = (const LV2_Atom*)buffer;
  if (lv2_atom_forge_is_object_type(&jalv->forge, atom->type)) {
    updating                   = true;
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*)buffer;
    if (obj->body.otype == jalv->urids.patch_Set) {
      const LV2_Atom_URID* property = NULL;
      const LV2_Atom*      value    = NULL;
      if (!patch_set_get(jalv, obj, &property, &value)) {
        property_changed(jalv, property->body, value);
      }
    } else if (obj->body.otype == jalv->urids.patch_Put) {
      const LV2_Atom_Object* body = NULL;
      if (!patch_put_get(jalv, obj, &body)) {
        LV2_ATOM_OBJECT_FOREACH (body, prop) {
          property_changed(jalv, prop->key, &prop->value);
        }
      }
    } else {
      jalv_log(JALV_LOG_ERR, "Unknown object type\n");
    }
    updating = false;
  }
}

static gboolean
scale_changed(GtkRange* range, gpointer data)
{
  set_float_control((const Control*)data, gtk_range_get_value(range));
  return FALSE;
}

static gboolean
spin_changed(GtkSpinButton* spin, gpointer data)
{
  const Control* control    = (const Control*)data;
  Controller*    controller = (Controller*)control->widget;
  GtkRange*      range      = GTK_RANGE(controller->control);
  const double   value      = gtk_spin_button_get_value(spin);
  if (differ_enough(gtk_range_get_value(range), value)) {
    gtk_range_set_value(range, value);
  }
  return FALSE;
}

static gboolean
log_scale_changed(GtkRange* range, gpointer data)
{
  set_float_control((const Control*)data, expf(gtk_range_get_value(range)));
  return FALSE;
}

static gboolean
log_spin_changed(GtkSpinButton* spin, gpointer data)
{
  const Control* control    = (const Control*)data;
  Controller*    controller = (Controller*)control->widget;
  GtkRange*      range      = GTK_RANGE(controller->control);
  const double   value      = gtk_spin_button_get_value(spin);
  if (differ_enough(gtk_range_get_value(range), logf(value))) {
    gtk_range_set_value(range, logf(value));
  }
  return FALSE;
}

static void
combo_changed(GtkComboBox* box, gpointer data)
{
  const Control* control = (const Control*)data;

  GtkTreeIter iter;
  if (gtk_combo_box_get_active_iter(box, &iter)) {
    GtkTreeModel* model = gtk_combo_box_get_model(box);
    GValue        value = G_VALUE_INIT;

    gtk_tree_model_get_value(model, &iter, 0, &value);
    const double v = g_value_get_float(&value);
    g_value_unset(&value);

    set_float_control(control, v);
  }
}

static gboolean
switch_changed(GtkSwitch* toggle_switch, gboolean state, gpointer data)
{
  (void)toggle_switch;
  (void)data;

  set_float_control((const Control*)data, state ? 1.0f : 0.0f);
  return FALSE;
}

static void
string_changed(GtkEntry* widget, gpointer data)
{
  const Control* control = (const Control*)data;
  const char*    string  = gtk_entry_get_text(widget);

  set_control(control, strlen(string) + 1, s_jalv->forge.String, string);
}

static void
file_changed(GtkFileChooserButton* widget, gpointer data)
{
  const Control* control = (const Control*)data;
  const char*    filename =
    gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));

  set_control(control, strlen(filename) + 1, s_jalv->forge.Path, filename);
}

static Controller*
new_controller(GtkSpinButton* spin, GtkWidget* control)
{
  Controller* controller = (Controller*)malloc(sizeof(Controller));
  controller->spin       = spin;
  controller->control    = control;
  return controller;
}

static Controller*
make_combo(Control* record, float value)
{
  GtkListStore* list_store = gtk_list_store_new(2, G_TYPE_FLOAT, G_TYPE_STRING);
  int           active     = -1;
  for (size_t i = 0; i < record->n_points; ++i) {
    const ScalePoint* point = &record->points[i];
    GtkTreeIter       iter;
    gtk_list_store_append(list_store, &iter);
    gtk_list_store_set(list_store, &iter, 0, point->value, 1, point->label, -1);
    if (fabsf(value - point->value) < FLT_EPSILON) {
      active = i;
    }
  }

  GtkWidget* combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(list_store));
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
  g_object_unref(list_store);

  gtk_widget_set_sensitive(combo, record->is_writable);
  gtk_widget_set_halign(combo, GTK_ALIGN_START);
  gtk_widget_set_hexpand(combo, FALSE);

  GtkCellRenderer* cell = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cell, TRUE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cell, "text", 1, NULL);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(combo), "changed", G_CALLBACK(combo_changed), record);
  }

  return new_controller(NULL, combo);
}

static Controller*
make_log_slider(Control* record, float value)
{
  const float min  = record->min;
  const float max  = record->max;
  const float lmin = logf(min);
  const float lmax = logf(max);
  const float ldft = logf(value);

  GtkWidget* const scale =
    gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lmin, lmax, 0.001);

  GtkWidget* const spin = gtk_spin_button_new_with_range(min, max, 0.000001);

  gtk_widget_set_sensitive(scale, record->is_writable);
  gtk_widget_set_sensitive(spin, record->is_writable);

  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
  gtk_range_set_value(GTK_RANGE(scale), ldft);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(scale), "value-changed", G_CALLBACK(log_scale_changed), record);
    g_signal_connect(
      G_OBJECT(spin), "value-changed", G_CALLBACK(log_spin_changed), record);
  }

  return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_slider(Control* record, float value)
{
  const float  min  = record->min;
  const float  max  = record->max;
  const double step = record->is_integer ? 1.0 : ((max - min) / 100.0);

  GtkWidget* const scale =
    gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);

  GtkWidget* const spin = gtk_spin_button_new_with_range(min, max, step);

  gtk_widget_set_sensitive(scale, record->is_writable);
  gtk_widget_set_sensitive(spin, record->is_writable);

  if (record->is_integer) {
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 0);
  } else {
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 7);
  }

  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
  gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  if (record->points) {
    for (size_t i = 0; i < record->n_points; ++i) {
      const ScalePoint* point = &record->points[i];

      gchar* str = g_markup_printf_escaped(
        "<span font_size=\"small\">%s</span>", point->label);
      gtk_scale_add_mark(GTK_SCALE(scale), point->value, GTK_POS_TOP, str);
    }
  }

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(scale), "value-changed", G_CALLBACK(scale_changed), record);
    g_signal_connect(
      G_OBJECT(spin), "value-changed", G_CALLBACK(spin_changed), record);
  }

  gtk_widget_set_halign(scale, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(scale, TRUE);
  return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_toggle_switch(Control* record, float value)
{
  GtkWidget* toggle_switch = gtk_switch_new();
  gtk_widget_set_halign(toggle_switch, GTK_ALIGN_START);
  gtk_widget_set_hexpand(toggle_switch, FALSE);

  gtk_widget_set_sensitive(toggle_switch, record->is_writable);

  if (value) {
    gtk_switch_set_active(GTK_SWITCH(toggle_switch), TRUE);
  }

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(toggle_switch), "state-set", G_CALLBACK(switch_changed), record);
  }

  return new_controller(NULL, toggle_switch);
}

static Controller*
make_entry(Control* control)
{
  GtkWidget* entry = gtk_entry_new();

  gtk_widget_set_sensitive(entry, control->is_writable);
  if (control->is_writable) {
    g_signal_connect(
      G_OBJECT(entry), "activate", G_CALLBACK(string_changed), control);
  }

  return new_controller(NULL, entry);
}

static Controller*
make_file_chooser(Control* record)
{
  GtkWidget* button =
    gtk_file_chooser_button_new("Open File", GTK_FILE_CHOOSER_ACTION_OPEN);

  gtk_widget_set_sensitive(button, record->is_writable);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(button), "file-set", G_CALLBACK(file_changed), record);
  }

  return new_controller(NULL, button);
}

static Controller*
make_controller(Control* control, float value)
{
  Controller* controller = NULL;

  if (control->is_toggle) {
    controller = make_toggle_switch(control, value);
  } else if (control->is_enumeration) {
    controller = make_combo(control, value);
  } else if (control->is_logarithmic) {
    controller = make_log_slider(control, value);
  } else {
    controller = make_slider(control, value);
  }

  return controller;
}

static GtkWidget*
new_label(const char* text, bool title, GtkAlign xalign, GtkAlign yalign)
{
  GtkWidget* const label = gtk_label_new(NULL);
  gchar* const     str =
    title
          ? g_markup_printf_escaped("<span font_weight=\"bold\">%s</span>", text)
          : g_markup_printf_escaped("%s:", text);

  gtk_widget_set_halign(label, xalign);
  gtk_widget_set_valign(label, yalign);
  gtk_label_set_markup(GTK_LABEL(label), str);

  g_free(str);
  return label;
}

static void
add_control_row(GtkWidget*  grid,
                int         row,
                const char* name,
                Controller* controller)
{
  GtkWidget* label = new_label(name, false, GTK_ALIGN_END, GTK_ALIGN_BASELINE);
  gtk_widget_set_margin_end(label, 8);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  if (controller->spin) {
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(controller->spin), 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), controller->control, 2, row, 1, 1);
  } else {
    gtk_grid_attach(GTK_GRID(grid), controller->control, 1, row, 2, 1);
  }
}

static int
control_group_cmp(const void* p1, const void* p2, void* ZIX_UNUSED(data))
{
  const Control* control1 = *(const Control* const*)p1;
  const Control* control2 = *(const Control* const*)p2;

  const int cmp = (control1->group && control2->group)
                    ? strcmp(lilv_node_as_string(control1->group),
                             lilv_node_as_string(control2->group))
                    : ((intptr_t)control1->group - (intptr_t)control2->group);

  return cmp;
}

static GtkWidget*
build_control_widget(Jalv* jalv, GtkWidget* window)
{
  GtkWidget* port_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(port_grid), 4);

  // Make an array of controls sorted by group
  GArray* controls = g_array_new(FALSE, TRUE, sizeof(Control*));
  for (unsigned i = 0; i < jalv->controls.n_controls; ++i) {
    g_array_append_vals(controls, &jalv->controls.controls[i], 1);
  }
  g_array_sort_with_data(controls, control_group_cmp, jalv);

  // Add controls in group order
  const LilvNode* last_group = NULL;
  int             n_rows     = 0;
  for (size_t i = 0; i < controls->len; ++i) {
    Control*    record     = g_array_index(controls, Control*, i);
    Controller* controller = NULL;
    LilvNode*   group      = record->group;

    // Check group and add new heading if necessary
    if (group && !lilv_node_equals(group, last_group)) {
      LilvNode* group_name =
        lilv_world_get(jalv->world, group, jalv->nodes.lv2_name, NULL);

      if (!group_name) {
        group_name =
          lilv_world_get(jalv->world, group, jalv->nodes.rdfs_label, NULL);
      }

      GtkWidget* group_label = new_label(lilv_node_as_string(group_name),
                                         true,
                                         GTK_ALIGN_START,
                                         GTK_ALIGN_BASELINE);

      lilv_node_free(group_name);
      gtk_grid_attach(GTK_GRID(port_grid), group_label, 0, n_rows, 3, 1);
      ++n_rows;
    }
    last_group = group;

    // Make control widget
    if (record->value_type == jalv->forge.String) {
      controller = make_entry(record);
    } else if (record->value_type == jalv->forge.Path) {
      controller = make_file_chooser(record);
    } else {
      controller = make_controller(record, record->def);
    }

    record->widget = controller;
    if (record->type == PORT) {
      jalv->ports[record->id.index].widget = controller;
    }
    if (controller) {
      // Add row to table for this controller
      add_control_row(port_grid,
                      n_rows++,
                      (record->label ? lilv_node_as_string(record->label)
                                     : lilv_node_as_uri(record->node)),
                      controller);

      // Set tooltip text from comment, if available
      LilvNode* comment = lilv_world_get(
        jalv->world, record->node, jalv->nodes.rdfs_comment, NULL);
      if (comment) {
        gtk_widget_set_tooltip_text(controller->control,
                                    lilv_node_as_string(comment));
      }
      lilv_node_free(comment);
    }
  }

  if (n_rows > 0) {
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_widget_set_margin_start(port_grid, 8);
    gtk_widget_set_margin_end(port_grid, 8);
    gtk_widget_set_halign(port_grid, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(port_grid, TRUE);
    gtk_widget_set_valign(port_grid, GTK_ALIGN_START);
    gtk_widget_set_vexpand(port_grid, FALSE);
    return port_grid;
  }

  gtk_widget_destroy(port_grid);
  GtkWidget* button = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(
    button, "clicked", G_CALLBACK(gtk_widget_destroy), window);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  return button;
}

static void
build_menu(Jalv* jalv, GtkWidget* window, GtkWidget* vbox)
{
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

  g_signal_connect(
    G_OBJECT(quit), "activate", G_CALLBACK(on_quit_activate), window);

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
    "Plugin Selector",
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

int
jalv_frontend_open(Jalv* jalv)
{
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  jalv->window      = window;

  s_jalv = jalv;

  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), jalv);

  set_window_title(jalv);

  GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

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
  (void)jalv;
  gtk_main_quit();
  s_jalv = NULL;
  return 0;
}
