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

#include "jalv_internal.h"

#include <QApplication>
#include <QPushButton>
#include <QMainWindow>
#include <QTimer>

static QApplication* app = NULL;

extern "C" {

void
jalv_init(int* argc, char*** argv)
{
	app = new QApplication(*argc, *argv, true);
}

LilvNode*
jalv_native_ui_type(Jalv* jalv)
{
	return lilv_new_uri(jalv->world,
	                    "http://lv2plug.in/ns/extensions/ui#Qt4UI");
}

class Timer : public QTimer {
public:
	Timer(Jalv* j) : jalv(j) {}

	void timerEvent(QTimerEvent* e) {
		jalv_emit_ui_events(jalv);
	}

private:
	Jalv* jalv;
};

int
jalv_open_ui(Jalv*         jalv,
             SuilInstance* instance)
{
	if (instance) {
		QWidget* widget = (QWidget*)suil_instance_get_widget(instance);
		widget->show();
	} else {
		QPushButton* button = new QPushButton("Close");
		button->show();
		QObject::connect(button, SIGNAL(clicked()), app, SLOT(quit()));
	}
	app->connect(app, SIGNAL(lastWindowClosed()), app, SLOT(quit()));

	Timer* timer = new Timer(jalv);
	timer->start(1000 / JALV_UI_UPDATE_HZ);

	int ret = app->exec();
	sem_post(jalv->done);
	return ret;
}

} // extern "C"
