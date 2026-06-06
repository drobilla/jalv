// Copyright 2007-2026 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "actions.h"

#include "jalv_gtk.h"
#include "menu.h"

#include "../jalv.h"
#include "../jalv_config.h"
#include "../log.h"
#include "../state.h"
#include "../types.h"

#include <glib-object.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>
#include <lilv/lilv.h>
#include <zix/allocator.h>
#include <zix/attributes.h>
#include <zix/filesystem.h>
#include <zix/path.h>
#include <zix/string_view.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

void
action_delete_preset(GSimpleAction* const ZIX_UNUSED(action),
                     GVariant* const      ZIX_UNUSED(parameter),
                     void* const          data)
{
  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;
  if (!jalv->preset) {
    return;
  }

  GtkWidget* dialog = gtk_dialog_new_with_buttons(
    "Delete Preset?",
    app->window,
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
    rebuild_preset_menu(jalv);
  }

  lilv_state_free(jalv->preset);
  jalv->preset = NULL;
  update_window(jalv);

  g_free(msg);
  gtk_widget_destroy(text);
  gtk_widget_destroy(dialog);
}

void
action_load_preset(GSimpleAction* const ZIX_UNUSED(action),
                   GVariant* const      parameter,
                   void* const          data)
{
  Jalv* const jalv = (Jalv*)data;

  const char* const uri = g_variant_get_string(parameter, NULL);
  if (!jalv->preset ||
      !!strcmp(lilv_node_as_string(lilv_state_get_uri(jalv->preset)), uri)) {
    LilvNode* const node = lilv_new_uri(jalv->world, uri);

    jalv_apply_preset(jalv, node);
    update_window(jalv);

    lilv_node_free(node);
  }
}

void
action_quit(GSimpleAction* const ZIX_UNUSED(action),
            GVariant* const      ZIX_UNUSED(parameter),
            void* const          data)
{
  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;
  if (app->window) {
    gtk_widget_destroy(GTK_WIDGET(app->window));
    app->window = NULL;
  }
}

void
action_save_preset(GSimpleAction* const ZIX_UNUSED(action),
                   GVariant* const      ZIX_UNUSED(parameter),
                   void* const          data)
{
  Jalv* const jalv = (Jalv*)data;
  if (!jalv->preset) {
    return;
  }

  // Use existing preset bundle path and label
  const char* const bundle = lilv_state_get_bundle_path(jalv->preset);
  const char* const label  = lilv_state_get_label(jalv->preset);
  if (!bundle) {
    return;
  }

  // Currently assumes simple presets in the format saved by Jalv

  // Ensure this is an "unpublished" preset with a file URI
  const LilvNode* const uri        = lilv_state_get_uri(jalv->preset);
  const char* const     uri_string = lilv_node_as_string(uri);
  if (!!strncmp(uri_string, "file:", 5)) {
    jalv_log(JALV_LOG_WARNING,
             "Unable to determine filename for preset <%s>",
             uri_string);
    return;
  }

  // Derive the filename from the preset URI
  char* const         path     = lilv_file_uri_parse(uri_string, NULL);
  const ZixStringView filename = zix_path_filename(path);

  // Unload old preset from model
  lilv_world_unload_resource(jalv->world, uri);

  // Save preset and reload into model
  char* const dir = zix_path_join(NULL, bundle, "");
  if (!jalv_save_preset(jalv, dir, NULL, label, filename.data)) {
    lilv_world_load_resource(jalv->world, lilv_state_get_uri(jalv->preset));
  } else {
    jalv_log(JALV_LOG_WARNING, "Error saving preset <%s>", uri_string);
  }

  zix_free(NULL, dir);
  lilv_free(path);
}

typedef struct {
  Jalv*      jalv;
  GString*   bundle_path;
  GtkWidget* save_button;
  GtkWidget* directory_button;
  GtkWidget* name_label;
  GtkWidget* name_entry;
  GtkWidget* overwrite_box;
  GtkWidget* overwrite_check_button;
  GtkWidget* overwrite_label;
} SavePresetWidgets;

static void
save_preset_widget_changed(const void* const ZIX_UNUSED(widget),
                           gpointer          user_data)
{
  SavePresetWidgets* const widgets = (SavePresetWidgets*)user_data;
  Jalv* const              jalv    = widgets->jalv;
  const char* const        home    = g_get_home_dir();
  const char* const        preset_name =
    gtk_entry_get_text(GTK_ENTRY(widgets->name_entry));

  if (!strlen(preset_name)) {
    g_string_assign(widgets->bundle_path, "");
    gtk_widget_set_sensitive(widgets->save_button, FALSE);
    return;
  }

  const char* const plugin_name = lilv_node_as_string(jalv->plugin_name);

  char* const long_name = g_strjoin(NULL, plugin_name, "_", preset_name, NULL);
  char* const base      = symbolify(long_name);
  char* const name      = g_strjoin(NULL, base, ".preset.lv2", NULL);
  char* const path      = g_build_filename(home, ".lv2", name, NULL);

  g_string_assign(widgets->bundle_path, (const char*)path);

  GtkToggleButton* const overwrite_toggle =
    GTK_TOGGLE_BUTTON(widgets->overwrite_check_button);

  const bool file_exists = (zix_file_type(path) != ZIX_FILE_TYPE_NONE);
  const bool overwrite   = gtk_toggle_button_get_active(overwrite_toggle);
  if (!file_exists) {
    gtk_label_set_markup(GTK_LABEL(widgets->overwrite_label), "");
  } else {
    LilvNode*   bundle_uri = lilv_new_file_uri(jalv->world, NULL, path);
    char* const message    = g_strdup_printf(
      "<a href=\"%s\">%s</a>", lilv_node_as_string(bundle_uri), base);

    gtk_label_set_markup(GTK_LABEL(widgets->overwrite_label), message);

    g_free(message);
    lilv_node_free(bundle_uri);
  }

  gtk_widget_set_sensitive(widgets->save_button, !file_exists || overwrite);
  gtk_widget_set_visible(widgets->overwrite_box, file_exists);

  g_free(path);
  g_free(name);
  free(base);
  g_free(long_name);
}

void
action_save_preset_as(GSimpleAction* const ZIX_UNUSED(action),
                      GVariant* const      ZIX_UNUSED(parameter),
                      void* const          data)
{
  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  GtkWidget* const dialog = gtk_dialog_new_with_buttons(
    "Save Preset",
    app->window,
    (GtkDialogFlags)(GTK_DIALOG_USE_HEADER_BAR | GTK_DIALOG_MODAL),
    "_Cancel",
    GTK_RESPONSE_CANCEL,
    "_OK",
    GTK_RESPONSE_ACCEPT,
    NULL);

  GtkWidget* const  content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkGrid* const    grid    = GTK_GRID(gtk_grid_new());
  SavePresetWidgets widgets = {
    jalv,
    g_string_new(""),
    gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT),
    gtk_file_chooser_button_new("Select an LV2 bundle _directory",
                                GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER),
    gtk_label_new("Name: "),
    gtk_entry_new(),
    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4),
    gtk_check_button_new_with_mnemonic("_Overwrite"),
    gtk_label_new(""),
  };

  // Set save button as default and have name entry activate it
  gtk_widget_set_sensitive(widgets.save_button, FALSE);
  gtk_window_set_default(GTK_WINDOW(dialog), widgets.save_button);
  gtk_entry_set_activates_default(GTK_ENTRY(widgets.name_entry), TRUE);
  gtk_widget_grab_default(widgets.save_button);

  g_signal_connect(G_OBJECT(widgets.name_entry),
                   "changed",
                   G_CALLBACK(save_preset_widget_changed),
                   &widgets);

  g_signal_connect(G_OBJECT(widgets.overwrite_check_button),
                   "toggled",
                   G_CALLBACK(save_preset_widget_changed),
                   &widgets);

  // Configure grid to fill window horizontally (with some margin)
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_widget_set_margin_start(GTK_WIDGET(grid), 16);
  gtk_widget_set_margin_end(GTK_WIDGET(grid), 16);
  gtk_widget_set_margin_top(GTK_WIDGET(grid), 16);
  gtk_widget_set_margin_bottom(GTK_WIDGET(grid), 16);
  gtk_widget_set_halign(GTK_WIDGET(grid), GTK_ALIGN_FILL);
  gtk_widget_set_valign(GTK_WIDGET(grid), GTK_ALIGN_START);
  gtk_widget_set_hexpand(GTK_WIDGET(grid), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(grid), FALSE);

  // Configure layout of content area widgets
  gtk_widget_set_halign(widgets.name_label, GTK_ALIGN_END);
  gtk_widget_set_halign(widgets.name_entry, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(widgets.name_entry, TRUE);
  gtk_widget_set_halign(widgets.overwrite_label, GTK_ALIGN_START);
  gtk_widget_set_halign(widgets.overwrite_check_button, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(widgets.overwrite_box),
                     widgets.overwrite_check_button,
                     TRUE,
                     TRUE,
                     0);
  gtk_box_pack_start(
    GTK_BOX(widgets.overwrite_box), widgets.overwrite_label, TRUE, TRUE, 0);

  // Attach widgets to grid
  gtk_grid_attach(grid, widgets.name_label, 0, 0, 1, 1);
  gtk_grid_attach(grid, widgets.name_entry, 1, 0, 1, 1);
  gtk_grid_attach(grid, widgets.overwrite_box, 0, 1, 2, 1);

  // Pack grid into content area and show initially visible contents
  gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(grid), FALSE, FALSE, 8);
  gtk_widget_show_all(dialog);
  gtk_widget_hide(widgets.overwrite_box);

  // Run the dialog and save the preset if the user accepted
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    const char* const bundle = widgets.bundle_path->str;
    const char* const label = gtk_entry_get_text(GTK_ENTRY(widgets.name_entry));

    char* const base     = symbolify(label);
    char* const filename = g_strjoin(NULL, base, ".ttl", NULL);
    char* const dir      = zix_path_join(NULL, bundle, "");
    if (!jalv_save_preset(jalv, dir, NULL, label, filename)) {
      // (Re)load bundle into the world
      LilvNode* ldir = lilv_new_file_uri(jalv->world, NULL, dir);
      lilv_world_unload_bundle(jalv->world, ldir);
      lilv_world_load_bundle(jalv->world, ldir);
      lilv_node_free(ldir);

      // Update UI
      rebuild_preset_menu(jalv);
      update_window(jalv);
    }

    zix_free(NULL, dir);
    g_free(filename);
    free(base);
  }

  g_string_free(widgets.bundle_path, TRUE);
  gtk_widget_destroy(dialog);
}

void
action_about(GSimpleAction* const ZIX_UNUSED(action),
             GVariant* const      ZIX_UNUSED(parameter),
             void* const          data)
{
  const char* const authors[] = {"David Robillard <d@drobilla.net>",
                                 "",
                                 "With contributions from:",
                                 "Amadeus Folego <amadeusfolego@gmail.com>",
                                 "Robin Gareus <robin@gareus.org>",
                                 "Nick Lanham <nick@afternight.org>",
                                 "Jaromír Mikes <mira.mikes@seznam.cz>",
                                 "Alexandros Theodotou <alex@zrythm.org>",
                                 "Timo Westkämper <timo.westkamper@gmail.com>",
                                 NULL};

  (void)data;
  gtk_show_about_dialog(NULL,
                        "program-name",
                        "Jalv",
                        "logo-icon-name",
                        "jalv",
                        "title",
                        "About Jalv",
                        "version",
                        JALV_VERSION,
                        "comments",
                        "Run an LV2 plugin",
                        "authors",
                        authors,
                        "website",
                        "http://drobilla.net/software/jalv",
                        NULL);
}
