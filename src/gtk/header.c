// Copyright 2023-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "header.h"

#include "menu.h"

#include "../jalv.h"
#include "../types.h"

#include <gio/gio.h>
#include <glib.h>
#include <lilv/lilv.h>

static GtkToolItem*
build_menu_button_item(GMenu* const menu)
{
  GtkWidget* const menu_button = gtk_menu_button_new();
  gtk_button_set_relief(GTK_BUTTON(menu_button), GTK_RELIEF_NONE);
  gtk_menu_button_set_direction(GTK_MENU_BUTTON(menu_button), GTK_ARROW_NONE);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                 G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(menu_button, "Show menu");

  GtkToolItem* menu_button_item = gtk_tool_item_new();
  gtk_container_add(GTK_CONTAINER(menu_button_item), menu_button);
  return menu_button_item;
}

GtkHeaderBar*
build_header_bar(Jalv* const jalv)
{
  GtkHeaderBar* const header_bar = GTK_HEADER_BAR(gtk_header_bar_new());

  gtk_header_bar_set_title(header_bar, lilv_node_as_string(jalv->plugin_name));
  gtk_header_bar_set_show_close_button(header_bar, TRUE);

  GMenu* const       menu             = build_main_menu(jalv);
  GtkToolItem* const menu_button_item = build_menu_button_item(menu);

  gtk_header_bar_pack_end(header_bar, GTK_WIDGET(menu_button_item));
  return header_bar;
}
