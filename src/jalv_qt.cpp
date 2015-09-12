/*
  Copyright 2007-2013 David Robillard <http://drobilla.net>

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

#include <stdio.h>
#include <math.h>

#include "jalv_internal.h"

#include "lv2/lv2plug.in/ns/ext/patch/patch.h"
#include "lv2/lv2plug.in/ns/ext/port-props/port-props.h"

#include <qglobal.h>

#if QT_VERSION >= 0x050000
#    include <QAction>
#    include <QApplication>
#    include <QDial>
#    include <QGroupBox>
#    include <QLabel>
#    include <QLayout>
#    include <QMainWindow>
#    include <QMenu>
#    include <QMenuBar>
#    include <QScrollArea>
#    include <QStyle>
#    include <QTimer>
#    include <QWidget>
#    include <QWindow>
#else
#    include <QtGui>
#endif

#define CONTROL_WIDTH 150
#define DIAL_STEPS    10000

static QApplication* app = NULL;

class FlowLayout : public QLayout
{
public:
	FlowLayout(QWidget* parent,
	           int      margin   = -1,
	           int      hSpacing = -1,
	           int      vSpacing = -1);

	FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);

	~FlowLayout();

	void             addItem(QLayoutItem* item);
	int              horizontalSpacing() const;
	int              verticalSpacing() const;
	Qt::Orientations expandingDirections() const;
	bool             hasHeightForWidth() const;
	int              heightForWidth(int) const;
	int              count() const;
	QLayoutItem*     itemAt(int index) const;
	QSize            minimumSize() const;
	void             setGeometry(const QRect &rect);
	QSize            sizeHint() const;
	QLayoutItem*     takeAt(int index);

private:
	int doLayout(const QRect &rect, bool testOnly) const;
	int smartSpacing(QStyle::PixelMetric pm) const;

	QList<QLayoutItem*> itemList;
	int m_hSpace;
	int m_vSpace;
};

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
	: QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
{
	setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::FlowLayout(int margin, int hSpacing, int vSpacing)
	: m_hSpace(hSpacing), m_vSpace(vSpacing)
{
	setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
	QLayoutItem* item;
	while ((item = takeAt(0))) {
		delete item;
	}
}

void
FlowLayout::addItem(QLayoutItem* item)
{
	itemList.append(item);
}

int
FlowLayout::horizontalSpacing() const
{
	if (m_hSpace >= 0) {
		return m_hSpace;
	} else {
		return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
	}
}

int
FlowLayout::verticalSpacing() const
{
	if (m_vSpace >= 0) {
		return m_vSpace;
	} else {
		return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
	}
}

int
FlowLayout::count() const
{
	return itemList.size();
}

QLayoutItem*
FlowLayout::itemAt(int index) const
{
	return itemList.value(index);
}

QLayoutItem*
FlowLayout::takeAt(int index)
{
	if (index >= 0 && index < itemList.size()) {
		return itemList.takeAt(index);
	} else {
		return 0;
	}
}

Qt::Orientations
FlowLayout::expandingDirections() const
{
	return 0;
}

bool
FlowLayout::hasHeightForWidth() const
{
	return true;
}

int
FlowLayout::heightForWidth(int width) const
{
	return doLayout(QRect(0, 0, width, 0), true);
}

void
FlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	doLayout(rect, false);
}

QSize
FlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize
FlowLayout::minimumSize() const
{
	QSize        size;
	QLayoutItem* item;
	foreach (item, itemList) {
		size = size.expandedTo(item->minimumSize());
	}

	return size + QSize(2 * margin(), 2 * margin());
}

int
FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	int left, top, right, bottom;
	getContentsMargins(&left, &top, &right, &bottom);

	QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
	int   x             = effectiveRect.x();
	int   y             = effectiveRect.y();
	int   lineHeight    = 0;

	QLayoutItem* item;
	foreach (item, itemList) {
		QWidget* wid = item->widget();

		int spaceX = horizontalSpacing();
		if (spaceX == -1) {
			spaceX = wid->style()->layoutSpacing(QSizePolicy::PushButton,
			                                     QSizePolicy::PushButton,
			                                     Qt::Horizontal);
		}
		int spaceY = verticalSpacing();
		if (spaceY == -1) {
			spaceY = wid->style()->layoutSpacing(QSizePolicy::PushButton,
			                                     QSizePolicy::PushButton,
			                                     Qt::Vertical);
		}

		int nextX = x + item->sizeHint().width() + spaceX;
		if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
			x          = effectiveRect.x();
			y          = y + lineHeight + spaceY;
			nextX      = x + item->sizeHint().width() + spaceX;
			lineHeight = 0;
		}

		if (!testOnly) {
			item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
		}

		x          = nextX;
		lineHeight = qMax(lineHeight, item->sizeHint().height());
	}
	return y + lineHeight - rect.y() + bottom;
}

int
FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
	QObject* parent = this->parent();
	if (!parent) {
		return -1;
	} else if (parent->isWidgetType()) {
		QWidget* pw = static_cast<QWidget*>(parent);
		return pw->style()->pixelMetric(pm, 0, pw);
	} else {
		return static_cast<QLayout*>(parent)->spacing();
	}
}

class PresetAction : public QAction
{
	Q_OBJECT

public:
	PresetAction(QObject* parent, Jalv* jalv, LilvNode* preset)
		: QAction(parent)
		, _jalv(jalv)
		, _preset(preset)
	{
		connect(this, SIGNAL(triggered()),
		        this, SLOT(presetChosen()));
	}

	Q_SLOT void presetChosen() {
		jalv_apply_preset(_jalv, _preset);
	}

private:
	Jalv*     _jalv;
	LilvNode* _preset;
};

typedef struct {
	Jalv*        jalv;
	struct Port* port;
} PortContainer;

class Control : public QGroupBox
{
	Q_OBJECT

public:
	Control(PortContainer portContainer, QWidget* parent = 0);

	Q_SLOT void dialChanged(int value);

	void setValue(float value);

	QDial* dial;

private:
	void    setRange(float min, float max);
	QString getValueLabel(float value);
	float   getValue();

	const LilvPlugin* plugin;
	struct Port*      port;

	QLabel* label;
	QString name;
	int     steps;
	float   max;
	float   min;
	bool    isInteger;
	bool    isEnum;
	bool    isLogarithmic;

	std::vector<float>           scalePoints;
	std::map<float, const char*> scaleMap;
};

#if QT_VERSION >= 0x050000
#    include "jalv_qt5_meta.hpp"
#else
#    include "jalv_qt4_meta.hpp"
#endif

extern "C" {

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	app = new QApplication(*argc, *argv, true);
	app->setStyleSheet("QGroupBox::title { subcontrol-position: top center }");

	return 0;
}

const char*
jalv_native_ui_type(Jalv* jalv)
{
#if QT_VERSION >= 0x050000
	return "http://lv2plug.in/ns/extensions/ui#Qt5UI";
#else
	return "http://lv2plug.in/ns/extensions/ui#Qt4UI";
#endif
}

int
jalv_ui_resize(Jalv* jalv, int width, int height)
{
	if (jalv->ui_instance && width > 0 && height > 0) {
		QWidget* widget = (QWidget*)suil_instance_get_widget(jalv->ui_instance);
		if (widget) {
			widget->resize(width, height);
		}
	}
	return 0;
}

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer)
{
	Control* control = (Control*)jalv->ports[port_index].widget;
	if (control) {
		control->setValue(*(const float*)buffer);
	}
}

class Timer : public QTimer
{
public:
	explicit Timer(Jalv* jalv) : _jalv(jalv) {}

	void timerEvent(QTimerEvent* e) {
		jalv_update(_jalv);
	}

private:
	Jalv* _jalv;
};

static int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
	QMenu*      menu  = (QMenu*)data;
	const char* label = lilv_node_as_string(title);

	QAction* action = new PresetAction(menu, jalv, lilv_node_duplicate(node));
	action->setText(label);
	menu->addAction(action);
	return 0;
}

Control::Control(PortContainer portContainer, QWidget* parent)
	: QGroupBox(parent)
	, dial(new QDial())
	, plugin(portContainer.jalv->plugin)
	, port(portContainer.port)
	, label(new QLabel())
{
	const LilvPort* lilvPort = port->lilv_port;
	LilvWorld*      world    = portContainer.jalv->world;

	LilvNode* lv2_integer     = lilv_new_uri(world, LV2_CORE__integer);
	LilvNode* lv2_toggled     = lilv_new_uri(world, LV2_CORE__toggled);
	LilvNode* lv2_enumeration = lilv_new_uri(world, LV2_CORE__enumeration);
	LilvNode* logarithmic     = lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
	LilvNode* rangeSteps      = lilv_new_uri(world, LV2_PORT_PROPS__rangeSteps);
	LilvNode* rdfs_comment    = lilv_new_uri(world, LILV_NS_RDFS "comment");

	LilvNode* nmin;
	LilvNode* nmax;
	LilvNode* ndef;
	lilv_port_get_range(plugin, lilvPort, &ndef, &nmin, &nmax);

	if (lilv_port_has_property(plugin, lilvPort, rangeSteps)) {
		steps = lilv_node_as_int(rangeSteps);
	} else {
		steps = DIAL_STEPS;
	}

	// Fill scalePoints Map
	LilvScalePoints* sp = lilv_port_get_scale_points(plugin, lilvPort);
	if (sp) {
		LILV_FOREACH(scale_points, s, sp) {
			const LilvScalePoint* p   = lilv_scale_points_get(sp, s);
			const LilvNode*       val = lilv_scale_point_get_value(p);
			if (!lilv_node_is_float(val)) {
				continue;
			}

			const float f = lilv_node_as_float(val);
			scalePoints.push_back(f);
			scaleMap[f] = lilv_node_as_string(lilv_scale_point_get_label(p));
		}

		lilv_scale_points_free(sp);
	}

	// Check port properties
	isLogarithmic = lilv_port_has_property(plugin, lilvPort, logarithmic);
	isInteger     = lilv_port_has_property(plugin, lilvPort, lv2_integer);
	isEnum        = lilv_port_has_property(plugin, lilvPort, lv2_enumeration);

	if (lilv_port_has_property(plugin, lilvPort, lv2_toggled)) {
		isInteger = true;

		if (!scaleMap[0]) {
			scaleMap[0] = "Off";
		}
		if (!scaleMap[1]) {
			scaleMap[1] = "On" ;
		}
	}

	// Find and set min, max and default values for port
	float defaultValue = ndef ? lilv_node_as_float(ndef) : port->control;
	setRange(lilv_node_as_float(nmin), lilv_node_as_float(nmax));
	setValue(defaultValue);

	// Fill layout
	QVBoxLayout* layout = new QVBoxLayout();
	layout->addWidget(label, 0, Qt::AlignHCenter);
	layout->addWidget(dial, 0, Qt::AlignHCenter);
	setLayout(layout);

	setMinimumWidth(CONTROL_WIDTH);
	setMaximumWidth(CONTROL_WIDTH);

	LilvNode* nname = lilv_port_get_name(plugin, lilvPort);
	name = QString("%1").arg(lilv_node_as_string(nname));

	// Handle long names
	if (fontMetrics().width(name) > CONTROL_WIDTH) {
		setTitle(fontMetrics().elidedText(name, Qt::ElideRight, CONTROL_WIDTH));
	} else {
		setTitle(name);
	}

	// Set tooltip if comment is available
	LilvNode* comment = lilv_port_get(plugin, lilvPort, rdfs_comment);
	if (comment) {
		QString* tooltip = new QString();
		tooltip->append(lilv_node_as_string(comment));
		setToolTip(*tooltip);
	}

	setFlat(true);

	connect(dial, SIGNAL(valueChanged(int)), this, SLOT(dialChanged(int)));

	lilv_node_free(nmin);
	lilv_node_free(nmax);
	lilv_node_free(ndef);
	lilv_node_free(nname);
	lilv_node_free(lv2_integer);
	lilv_node_free(lv2_toggled);
	lilv_node_free(lv2_enumeration);
	lilv_node_free(logarithmic);
	lilv_node_free(rangeSteps);
	lilv_node_free(comment);
}

void
Control::setValue(float value)
{
	float step;

	if (isInteger) {
		step = value;
	} else if (isEnum) {
		step = (std::find(scalePoints.begin(), scalePoints.end(), value)
		        - scalePoints.begin());
	} else if (isLogarithmic) {
		step = steps * log(value / min) / log(max / min);
	} else {
		step = value * steps;
	}

	dial->setValue(step);
	label->setText(getValueLabel(value));
}

QString
Control::getValueLabel(float value)
{
	if (scaleMap[value]) {
		if (fontMetrics().width(scaleMap[value]) > CONTROL_WIDTH) {
			label->setToolTip(scaleMap[value]);
			return fontMetrics().elidedText(QString(scaleMap[value]),
			                                Qt::ElideRight,
			                                CONTROL_WIDTH);
		}
		return scaleMap[value];
	}

	return QString("%1").arg(value);
}

void
Control::setRange(float minRange, float maxRange)
{
	min = minRange;
	max = maxRange;

	if (isLogarithmic) {
		minRange = 1;
		maxRange = steps;
	} else if (isEnum) {
		minRange = 0;
		maxRange = scalePoints.size() - 1;
	} else if (!isInteger) {
		minRange *= steps;
		maxRange *= steps;
	}

	dial->setRange(minRange, maxRange);
}

float
Control::getValue()
{
	if (isEnum) {
		return scalePoints[dial->value()];
	} else if (isInteger) {
		return dial->value();
	} else if (isLogarithmic) {
		return min * pow(max / min, (float)dial->value() / steps);
	} else {
		return (float)dial->value() / steps;
	}
}

void
Control::dialChanged(int dialValue)
{
	float value = getValue();

	label->setText(getValueLabel(value));
	port->control = value;
}

static bool
portGroupLessThan(const PortContainer &p1, const PortContainer &p2)
{
	Jalv*           jalv  = p1.jalv;
	const LilvPort* port1 = p1.port->lilv_port;
	const LilvPort* port2 = p2.port->lilv_port;

	LilvNode* group1 = lilv_port_get(
		jalv->plugin, port1, jalv->nodes.pg_group);
	LilvNode* group2 = lilv_port_get(
		jalv->plugin, port2, jalv->nodes.pg_group);

	const int cmp = (group1 && group2)
		? strcmp(lilv_node_as_string(group1), lilv_node_as_string(group2))
		: ((intptr_t)group1 - (intptr_t)group2);

	lilv_node_free(group2);
	lilv_node_free(group1);

	return cmp < 0;
}

static QWidget*
build_control_widget(Jalv* jalv)
{
	const LilvPlugin* plugin = jalv->plugin;
	LilvWorld*        world  = jalv->world;

	LilvNode* pprop_notOnGUI = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);

	QList<PortContainer> portContainers;
	for (unsigned i = 0; i < jalv->num_ports; ++i) {
		if (!jalv->opts.show_hidden &&
		    lilv_port_has_property(plugin, jalv->ports[i].lilv_port, pprop_notOnGUI)) {
			continue;
		}

		if (jalv->ports[i].type == TYPE_CONTROL) {
			PortContainer portContainer;
			portContainer.jalv = jalv;
			portContainer.port = &jalv->ports[i];
			portContainers.append(portContainer);
		}
	}

	qSort(portContainers.begin(), portContainers.end(), portGroupLessThan);

	QWidget*    grid       = new QWidget();
	FlowLayout* flowLayout = new FlowLayout();
	QLayout*    layout     = flowLayout;

	LilvNode*    lastGroup = NULL;
	QHBoxLayout* groupLayout;
	for (int i = 0; i < portContainers.count(); ++i) {
		PortContainer portContainer = portContainers[i];
		Port*         port          = portContainer.port;

		Control*  control = new Control(portContainer);
		LilvNode* group   = lilv_port_get(
			plugin, port->lilv_port, jalv->nodes.pg_group);
		if (group) {
			if (!lilv_node_equals(group, lastGroup)) {
				/* Group has changed */
				LilvNode* groupName = lilv_world_get(
					world, group, jalv->nodes.lv2_name, NULL);
				QGroupBox* groupBox = new QGroupBox(lilv_node_as_string(groupName));

				groupLayout = new QHBoxLayout();
				groupBox->setLayout(groupLayout);
				layout->addWidget(groupBox);
			}

			groupLayout->addWidget(control);
		} else {
			layout->addWidget(control);
		}
		lastGroup = group;

		uint32_t index = lilv_port_get_index(plugin, port->lilv_port);
		jalv->ports[index].widget = control;
	}

	grid->setLayout(layout);

	lilv_node_free(pprop_notOnGUI);

	return grid;
}

int
jalv_open_ui(Jalv* jalv)
{
	QMainWindow* win          = new QMainWindow();
	QMenu*       file_menu    = win->menuBar()->addMenu("&File");
	QMenu*       presets_menu = win->menuBar()->addMenu("&Presets");
	QAction*     quit_action  = new QAction("&Quit", win);

	QObject::connect(quit_action, SIGNAL(triggered()), win, SLOT(close()));
	quit_action->setShortcuts(QKeySequence::Quit);
	quit_action->setStatusTip("Quit Jalv");
	file_menu->addAction(quit_action);
	jalv->has_ui = true;

	jalv_load_presets(jalv, add_preset_to_menu, presets_menu);

	if (jalv->ui && !jalv->opts.generic_ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(jalv), win);
	}

	QWidget* widget;
	if (jalv->ui_instance) {
		widget = (QWidget*)suil_instance_get_widget(jalv->ui_instance);
	} else {
		QWidget* controlWidget = build_control_widget(jalv);

		widget = new QScrollArea();
		((QScrollArea*)widget)->setWidget(controlWidget);
		((QScrollArea*)widget)->setWidgetResizable(true);
		widget->setMinimumWidth(800);
		widget->setMinimumHeight(600);
	}

	LilvNode* name = lilv_plugin_get_name(jalv->plugin);
	win->setWindowTitle(lilv_node_as_string(name));
	lilv_node_free(name);

	win->setCentralWidget(widget);
	app->connect(app, SIGNAL(lastWindowClosed()), app, SLOT(quit()));

	win->show();

	Timer* timer = new Timer(jalv);
	timer->start(1000 / jalv->ui_update_hz);

	int ret = app->exec();
	zix_sem_post(jalv->done);
	return ret;
}

int
jalv_close_ui(Jalv* jalv)
{
	app->quit();
	return 0;
}

}  // extern "C"
