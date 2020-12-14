/*
  Copyright 2007-2016 David Robillard <http://drobilla.net>

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

#include "jalv_internal.h"

#include "lv2/core/attributes.h"
#include "suil/suil.h"
#include "zix/sem.h"

LV2_DISABLE_DEPRECATION_WARNINGS
#include <glib.h>
#include <gtkmm/button.h>
#include <gtkmm/main.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
LV2_RESTORE_WARNINGS

#include <cstdint>

static Gtk::Main* jalv_gtk_main = nullptr;

int
jalv_init(int* argc, char*** argv, JalvOptions*)
{
	jalv_gtk_main = new Gtk::Main(*argc, *argv);
	return 0;
}

const char*
jalv_native_ui_type(void)
{
	return "http://lv2plug.in/ns/extensions/ui#GtkUI";
}

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer)
{
	if (jalv->ui_instance) {
		suil_instance_port_event(jalv->ui_instance, port_index,
		                         buffer_size, protocol, buffer);
	}
}

bool
jalv_discover_ui(Jalv*)
{
	return true;
}

float
jalv_ui_refresh_rate(Jalv*)
{
	return 30.0f;
}

int
jalv_open_ui(Jalv* jalv)
{
	Gtk::Window* window = new Gtk::Window();

	if (jalv->ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(), nullptr);
	}

	if (jalv->ui_instance) {
		GtkWidget* widget = static_cast<GtkWidget*>(
		    suil_instance_get_widget(jalv->ui_instance));

		Gtk::Widget* widgetmm = Glib::wrap(widget);
		window->add(*Gtk::manage(widgetmm));
		widgetmm->show_all();

		g_timeout_add(1000 / jalv->ui_update_hz,
		              reinterpret_cast<GSourceFunc>(jalv_update),
		              jalv);
	} else {
		Gtk::Button* button = Gtk::manage(new Gtk::Button("Close"));
		window->add(*Gtk::manage(button));
	}

	jalv_init_ui(jalv);

	window->set_resizable(jalv_ui_is_resizable(jalv));
	window->show_all();

	Gtk::Main::run(*window);

	delete window;
	delete jalv_gtk_main;
	zix_sem_post(&jalv->done);

	return 0;
}

int
jalv_close_ui(Jalv*)
{
	Gtk::Main::quit();
	return 0;
}
