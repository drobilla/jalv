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

static void destroy(GtkWidget* widget,
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

int
jalv_open_ui(Jalv*         jalv,
             SuilInstance* instance)
{
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	g_signal_connect(window, "destroy",
	                 G_CALLBACK(destroy), NULL);

	gtk_window_set_title(GTK_WINDOW(window),
	                     lilv_node_as_string(lilv_plugin_get_name(jalv->plugin)));

	GtkWidget* vbox      = gtk_vbox_new(FALSE, 0);
	GtkWidget* menubar   = gtk_menu_bar_new();
	GtkWidget* file      = gtk_menu_item_new_with_mnemonic("_File");
	GtkWidget* file_menu = gtk_menu_new();
	GtkWidget* quit      = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), file_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit);
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);
	gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(quit), "activate",
	                 G_CALLBACK(gtk_main_quit), NULL);

	GtkWidget* alignment = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
	gtk_box_pack_start(GTK_BOX(vbox), alignment, TRUE, TRUE, 0);
	
	if (instance) {
		GtkWidget* widget = (GtkWidget*)suil_instance_get_widget(instance);
		gtk_container_add(GTK_CONTAINER(alignment), widget);
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

	g_timeout_add(1000 / JALV_UI_UPDATE_HZ,
	              (GSourceFunc)jalv_emit_ui_events, jalv);

	gtk_main();
	sem_post(jalv->done);
	return 0;
}
