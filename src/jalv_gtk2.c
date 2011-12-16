/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <gtk/gtk.h>

#include "jalv_internal.h"

static void
on_window_destroy(GtkWidget* widget,
                  gpointer   data)
{
	gtk_main_quit();
}

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	GOptionEntry entries[] = {
		{ "uuid", 'u', 0, G_OPTION_ARG_STRING, &opts->uuid,
		  "UUID for Jack session restoration", "UUID" },
		{ "load", 'l', 0, G_OPTION_ARG_STRING, &opts->load,
		  "Load state from save directory", "DIR" },
		{ 0,0,0,0,0,0,0 } };
	GError* error = NULL;
	const int err = gtk_init_with_args(
		argc, argv,
		"PLUGIN_URI - Run an LV2 plugin as a Jack application",
		entries, NULL, &error);

	if (!err) {
		fprintf(stderr, "%s\n", error->message);
	}

	return !err;
}

LilvNode*
jalv_native_ui_type(Jalv* jalv)
{
	return lilv_new_uri(jalv->world,
	                    "http://lv2plug.in/ns/extensions/ui#GtkUI");
}

static void
on_save_activate(GtkWidget* widget, void* ptr)
{
	Jalv* jalv = (Jalv*)ptr;
	GtkWidget* dialog = gtk_file_chooser_dialog_new(
		"Save State",
		NULL, // FIXME: parent
		GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
		NULL);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		char* base     = g_build_filename(filename, "/", NULL);
		fprintf(stderr, "SAVE TO %s\n", base);
		jalv_save(jalv, base);
		g_free(filename);
		g_free(base);
	}

	gtk_widget_destroy(dialog);
}

static void
on_quit_activate(GtkWidget* widget, gpointer data)
{
	GtkWidget* window = (GtkWidget*)data;
	gtk_widget_destroy(window);
}

typedef struct {
	Jalv*     jalv;
	LilvNode* preset;
} PresetRecord;

static void
on_preset_activate(GtkWidget* widget, gpointer data)
{
	PresetRecord* record = (PresetRecord*)data;
	jalv_apply_preset(record->jalv, record->preset);
}

static void
on_preset_destroy(gpointer data, GClosure* closure)
{
	PresetRecord* record = (PresetRecord*)data;
	lilv_node_free(record->preset);
	free(record);
}

static int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
	GtkWidget*  presets_menu = GTK_WIDGET(data);
	const char* label        = lilv_node_as_string(title);
	GtkWidget*  item         = gtk_menu_item_new_with_label(label);

	PresetRecord* record = (PresetRecord*)malloc(sizeof(PresetRecord));
	record->jalv   = jalv;
	record->preset = lilv_node_duplicate(node);

	g_signal_connect_data(G_OBJECT(item), "activate",
	                      G_CALLBACK(on_preset_activate),
	                      record, on_preset_destroy,
	                      0);

	gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu), item);
	return 0;
}

int
jalv_ui_resize(Jalv* jalv, int width, int height)
{
	if (jalv->ui_instance) {
		GtkWidget* widget = (GtkWidget*)suil_instance_get_widget(jalv->ui_instance);
		if (widget) {
			gtk_widget_set_size_request(GTK_WIDGET(widget), width, height);
		}
	}
	return 0;
}

int
jalv_open_ui(Jalv*         jalv,
             SuilInstance* instance)
{
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	g_signal_connect(window, "destroy",
	                 G_CALLBACK(on_window_destroy), NULL);

	gtk_window_set_title(GTK_WINDOW(window),
	                     lilv_node_as_string(lilv_plugin_get_name(jalv->plugin)));

	GtkAccelGroup* accels = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(window), accels);

	GtkWidget* vbox      = gtk_vbox_new(FALSE, 0);
	GtkWidget* menu_bar  = gtk_menu_bar_new();
	GtkWidget* file      = gtk_menu_item_new_with_mnemonic("_File");
	GtkWidget* file_menu = gtk_menu_new();
	GtkWidget* save      = gtk_image_menu_item_new_from_stock(GTK_STOCK_SAVE, accels);
	GtkWidget* quit      = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, accels);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), file_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file);

	GtkWidget* presets      = gtk_menu_item_new_with_mnemonic("_Presets");
	GtkWidget* presets_menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(presets), presets_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), presets);

	jalv_load_presets(jalv, add_preset_to_menu, presets_menu);

	gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(quit), "activate",
	                 G_CALLBACK(on_quit_activate), window);

	g_signal_connect(G_OBJECT(save), "activate",
	                 G_CALLBACK(on_save_activate), jalv);

	GtkWidget* alignment = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
	gtk_box_pack_start(GTK_BOX(vbox), alignment, TRUE, TRUE, 0);
	
	if (instance) {
		GtkWidget* widget = (GtkWidget*)suil_instance_get_widget(instance);
		gtk_container_add(GTK_CONTAINER(alignment), widget);

		g_timeout_add(1000 / JALV_UI_UPDATE_HZ,
		              (GSourceFunc)jalv_emit_ui_events, jalv);

		jalv_ui_resize(jalv, jalv->ui_width, jalv->ui_height);
	} else {
		GtkWidget* button = gtk_button_new_with_label("Close");

		g_signal_connect_swapped(button, "clicked",
		                         G_CALLBACK(gtk_widget_destroy),
		                         window);

		gtk_container_add(GTK_CONTAINER(alignment), button);
	}

	// TODO: Check UI properties for resizable
	gtk_window_set_resizable(GTK_WINDOW(window), false);

	gtk_container_add(GTK_CONTAINER(window), vbox);
	gtk_widget_show_all(window);

	gtk_main();
	sem_post(jalv->done);
	return 0;
}
