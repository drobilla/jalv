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

#include <gtk/gtk.h>
#include <lilv/lilv.h>
#include <zix/allocator.h>
#include <zix/attributes.h>
#include <zix/path.h>
#include <zix/string_view.h>

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
             "Unable to determine filename for preset <%s>\n",
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
    jalv_log(JALV_LOG_WARNING, "Error saving preset <%s>\n", uri_string);
  }

  zix_free(NULL, dir);
  lilv_free(path);
}

void
action_save_preset_as(GSimpleAction* const ZIX_UNUSED(action),
                      GVariant* const      ZIX_UNUSED(parameter),
                      void* const          data)
{
  Jalv* const jalv = (Jalv*)data;
  App* const  app  = (App*)jalv->app;

  GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Preset",
                                                  app->window,
                                                  GTK_FILE_CHOOSER_ACTION_SAVE,
                                                  "_Cancel",
                                                  GTK_RESPONSE_REJECT,
                                                  "_Save",
                                                  GTK_RESPONSE_ACCEPT,
                                                  NULL);

  char* dot_lv2 = g_build_filename(g_get_home_dir(), ".lv2", NULL);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dot_lv2);
  g_free(dot_lv2);

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
    const char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    const char* uri  = gtk_entry_get_text(GTK_ENTRY(uri_entry));
    const char* prefix = "";
    const char* sep    = "";
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_prefix))) {
      prefix = lilv_node_as_string(jalv->plugin_name);
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

    // Update UI
    rebuild_preset_menu(jalv);
    update_window(jalv);

    g_free(dir);
    g_free(file);
    g_free(bundle);
    free(sprefix);
    free(sym);
    g_free(basename);
    g_free(dirname);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
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
