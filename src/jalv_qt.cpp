/*
  Copyright 2007-2016 David Robillard <d@drobilla.net>

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

#include <pthread.h>

#include "jalv_internal.h"

#include "lilv/lilv.h"
#include "suil/suil.h"
#include "zix/sem.h"

#include <QtGlobal>

#if QT_VERSION >= 0x050000
#    include <QAction>
#    include <QApplication>
#    include <QDial>
#    include <QFontMetrics>
#    include <QGroupBox>
#    include <QGuiApplication>
#    include <QHBoxLayout>
#    include <QKeySequence>
#    include <QLabel>
#    include <QLayout>
#    include <QLayoutItem>
#    include <QList>
#    include <QMainWindow>
#    include <QMenu>
#    include <QMenuBar>
#    include <QObject>
#    include <QPoint>
#    include <QRect>
#    include <QScreen>
#    include <QScrollArea>
#    include <QSize>
#    include <QSizePolicy>
#    include <QString>
#    include <QStyle>
#    include <QTimer>
#    include <QVBoxLayout>
#    include <QWidget>
#    include <QtCore>
#else
#    include <QtGui>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#define CONTROL_WIDTH 150
#define DIAL_STEPS    10000

static QApplication* app = nullptr;

class FlowLayout : public QLayout
{
public:
	explicit FlowLayout(QWidget* parent,
	                    int      margin,
	                    int      hSpacing,
	                    int      vSpacing);

	explicit FlowLayout(int margin, int hSpacing, int vSpacing);

	FlowLayout(const FlowLayout&) = delete;
	FlowLayout& operator=(const FlowLayout&) = delete;

	FlowLayout(FlowLayout&&) = delete;
	FlowLayout&& operator=(FlowLayout&&) = delete;

	~FlowLayout() override;

	void             addItem(QLayoutItem* item) override;
	int              horizontalSpacing() const;
	int              verticalSpacing() const;
	Qt::Orientations expandingDirections() const override;
	bool             hasHeightForWidth() const override;
	int              heightForWidth(int) const override;
	int              count() const override;
	QLayoutItem*     itemAt(int index) const override;
	QSize            minimumSize() const override;
	void             setGeometry(const QRect &rect) override;
	QSize            sizeHint() const override;
	QLayoutItem*     takeAt(int index) override;

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
	QLayoutItem* item = nullptr;
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
		return nullptr;
	}
}

Qt::Orientations
FlowLayout::expandingDirections() const
{
	return Qt::Orientations();
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
	QSize        size = {};
	QLayoutItem* item = nullptr;
	foreach (item, itemList) {
		size = size.expandedTo(item->minimumSize());
	}

	return size + QSize(2 * margin(), 2 * margin());
}

int
FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
	getContentsMargins(&left, &top, &right, &bottom);

	QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
	int   x             = effectiveRect.x();
	int   y             = effectiveRect.y();
	int   lineHeight    = 0;

	QLayoutItem* item = nullptr;
	foreach (item, itemList) {
		QWidget* wid = item->widget();

		int spaceX = horizontalSpacing();
		if (spaceX == -1) {
			spaceX = wid->style()->layoutSpacing(QSizePolicy::PushButton,
			                                     QSizePolicy::PushButton,
			                                     Qt::Horizontal,
			                                     nullptr,
			                                     nullptr);
		}
		int spaceY = verticalSpacing();
		if (spaceY == -1) {
			spaceY = wid->style()->layoutSpacing(QSizePolicy::PushButton,
			                                     QSizePolicy::PushButton,
			                                     Qt::Vertical,
			                                     nullptr,
			                                     nullptr);
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
		return pw->style()->pixelMetric(pm, nullptr, pw);
	} else {
		return static_cast<QLayout*>(parent)->spacing();
	}
}

class PresetAction : public QAction
{
	Q_OBJECT // NOLINT

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
	Q_OBJECT // NOLINT

public:
	explicit Control(PortContainer portContainer, QWidget* parent);

	Q_SLOT void dialChanged(int value);

	void setValue(float value);

private:
	void    setRange(float min, float max);
	QString getValueLabel(float value);
	float   getValue();
	int     stringWidth(const QString& str);

	QDial*            dial;
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
#    include "jalv_qt5_meta.hpp" // IWYU pragma: keep
#else
#    include "jalv_qt4_meta.hpp" // IWYU pragma: keep
#endif

extern "C" {

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	
	char cwd[256];
	if (getcwd(cwd, sizeof(cwd)-1) != NULL) {
		opts->preset_path = jalv_strdup(cwd);
	} else {
		opts->preset_path = jalv_strdup("./");
	}

	app = new QApplication(*argc, *argv, true);
	app->setStyleSheet("QGroupBox::title { subcontrol-position: top center }");

	return 0;
}

const char*
jalv_native_ui_type(void)
{
#if QT_VERSION >= 0x050000
	return "http://lv2plug.in/ns/extensions/ui#Qt5UI";
#else
	return "http://lv2plug.in/ns/extensions/ui#Qt4UI";
#endif
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
	} else {
		Control* control =
		    static_cast<Control*>(jalv->ports[port_index].widget);
		if (control) {
			control->setValue(*static_cast<const float*>(buffer));
		}
	}
}

class Timer : public QTimer
{
public:
	explicit Timer(Jalv* jalv) : _jalv(jalv) {}

	void timerEvent(QTimerEvent*) override {
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
	QMenu*      menu  = static_cast<QMenu*>(data);
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
    , max(1.0f)
    , min(0.0f)
    , isInteger(false)
    , isEnum(false)
    , isLogarithmic(false)
{
	JalvNodes*      nodes    = &portContainer.jalv->nodes;
	const LilvPort* lilvPort = port->lilv_port;

	LilvNode* nmin = nullptr;
	LilvNode* nmax = nullptr;
	LilvNode* ndef = nullptr;
	lilv_port_get_range(plugin, lilvPort, &ndef, &nmin, &nmax);

	LilvNode* stepsNode = lilv_port_get(plugin, lilvPort, nodes->pprops_rangeSteps);
	if (lilv_node_is_int(stepsNode)) {
		steps = std::max(lilv_node_as_int(stepsNode), 2);
	} else {
		steps = DIAL_STEPS;
	}
	lilv_node_free(stepsNode);

	// Fill scalePoints Map
	LilvScalePoints* sp = lilv_port_get_scale_points(plugin, lilvPort);
	if (sp) {
		LILV_FOREACH(scale_points, s, sp) {
			const LilvScalePoint* p   = lilv_scale_points_get(sp, s);
			const LilvNode*       val = lilv_scale_point_get_value(p);
			if (!lilv_node_is_float(val) && !lilv_node_is_int(val)) {
				continue;
			}

			const float f = lilv_node_as_float(val);
			scalePoints.push_back(f);
			scaleMap[f] = lilv_node_as_string(lilv_scale_point_get_label(p));
		}

		lilv_scale_points_free(sp);
	}

	// Check port properties
	isLogarithmic = lilv_port_has_property(plugin, lilvPort, nodes->pprops_logarithmic);
	isInteger     = lilv_port_has_property(plugin, lilvPort, nodes->lv2_integer);
	isEnum        = lilv_port_has_property(plugin, lilvPort, nodes->lv2_enumeration);

	if (lilv_port_has_property(plugin, lilvPort, nodes->lv2_toggled)) {
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
	if (stringWidth(name) > CONTROL_WIDTH) {
		setTitle(fontMetrics().elidedText(name, Qt::ElideRight, CONTROL_WIDTH));
	} else {
		setTitle(name);
	}

	// Set tooltip if comment is available
	LilvNode* comment = lilv_port_get(plugin, lilvPort, nodes->rdfs_comment);
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
	lilv_node_free(comment);
}

void
Control::setValue(float value)
{
	float step = 0.0f;

	if (isInteger) {
		step = value;
	} else if (isEnum) {
		step = (std::find(scalePoints.begin(), scalePoints.end(), value)
		        - scalePoints.begin());
	} else if (isLogarithmic) {
		step = steps * logf(value / min) / logf(max / min);
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
		if (stringWidth(scaleMap[value]) > CONTROL_WIDTH) {
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
		return min * powf(max / min, (float)dial->value() / (steps - 1));
	} else {
		return (float)dial->value() / steps;
	}
}

int
Control::stringWidth(const QString& str)
{
#if QT_VERSION >= 0x050B00
	return fontMetrics().horizontalAdvance(str);
#else
	return fontMetrics().width(str);
#endif
}

void
Control::dialChanged(int)
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
		: (intptr_t(group1) - intptr_t(group2));

	lilv_node_free(group2);
	lilv_node_free(group1);

	return cmp < 0;
}

static QWidget*
build_control_widget(Jalv* jalv)
{
	const LilvPlugin* plugin = jalv->plugin;
	LilvWorld*        world  = jalv->world;

	QList<PortContainer> portContainers;
	for (unsigned i = 0; i < jalv->num_ports; ++i) {
		if (!jalv->opts.show_hidden &&
		    lilv_port_has_property(plugin, jalv->ports[i].lilv_port,
		                           jalv->nodes.pprops_notOnGUI)) {
			continue;
		}

		if (jalv->ports[i].type == TYPE_CONTROL) {
			PortContainer portContainer;
			portContainer.jalv = jalv;
			portContainer.port = &jalv->ports[i];
			portContainers.append(portContainer);
		}
	}

	std::sort(portContainers.begin(), portContainers.end(), portGroupLessThan);

	QWidget*    grid       = new QWidget();
	FlowLayout* flowLayout = new FlowLayout(-1, -1, -1);
	QLayout*    layout     = flowLayout;

	LilvNode*    lastGroup   = nullptr;
	QHBoxLayout* groupLayout = nullptr;
	for (int i = 0; i < portContainers.count(); ++i) {
		PortContainer portContainer = portContainers[i];
		Port*         port          = portContainer.port;

		Control*  control = new Control(portContainer, nullptr);
		LilvNode* group   = lilv_port_get(
			plugin, port->lilv_port, jalv->nodes.pg_group);
		if (group) {
			if (!lilv_node_equals(group, lastGroup)) {
				/* Group has changed */
				LilvNode* groupName = lilv_world_get(
					world, group, jalv->nodes.lv2_name, nullptr);
				if (!groupName) {
					groupName = lilv_world_get(
					        world, group, jalv->nodes.rdfs_label, nullptr);
				}

				QGroupBox* groupBox = new QGroupBox(lilv_node_as_string(groupName));

				groupLayout = new QHBoxLayout();
				groupBox->setLayout(groupLayout);
				layout->addWidget(groupBox);
			}

			groupLayout->addWidget(control);
		} else {
			layout->addWidget(control);
		}
		lilv_node_free(lastGroup);
		lastGroup = group;

		uint32_t index = lilv_port_get_index(plugin, port->lilv_port);
		jalv->ports[index].widget = control;
	}
	lilv_node_free(lastGroup);

	grid->setLayout(layout);

	return grid;
}

bool
jalv_discover_ui(Jalv*)
{
	return true;
}

float
jalv_ui_refresh_rate(Jalv*)
{
#if QT_VERSION >= 0x050000
	return (float)QGuiApplication::primaryScreen()->refreshRate();
#else
	return 30.0f;
#endif
}

static void
set_window_title(Jalv* jalv)
{
	LilvNode* name = lilv_plugin_get_name(jalv->plugin);
	const char* plugin = lilv_node_as_string(name);
	QMainWindow* win = (QMainWindow*) jalv->window;
	if (jalv->preset) {
		const char* preset_label = lilv_state_get_label(jalv->preset);
		char *title = (char *)malloc(strlen(plugin)+strlen(preset_label)+4);
		sprintf(title, "%s - %s", plugin, preset_label);
		win->setWindowTitle(title);
		free(title);
	} else {
		win->setWindowTitle(plugin);
	}
	lilv_node_free(name);
}


pthread_t init_cli_thread(Jalv* jalv);

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

	jalv_load_presets(jalv, add_preset_to_menu, presets_menu);

	if (jalv->ui && !jalv->opts.generic_ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(), win);
	}

	QWidget* widget = nullptr;
	if (jalv->ui_instance) {
		widget =
		    static_cast<QWidget*>(suil_instance_get_widget(jalv->ui_instance));
	} else {
		QWidget* controlWidget = build_control_widget(jalv);

		widget = new QScrollArea();
		static_cast<QScrollArea*>(widget)->setWidget(controlWidget);
		static_cast<QScrollArea*>(widget)->setWidgetResizable(true);
		widget->setMinimumWidth(800);
		widget->setMinimumHeight(600);
	}

	jalv->window = win;
	set_window_title(jalv);

	win->setCentralWidget(widget);
	app->connect(app, SIGNAL(lastWindowClosed()), app, SLOT(quit()));

	jalv_init_ui(jalv);

	win->show();
	if (jalv->ui_instance && !jalv_ui_is_resizable(jalv)) {
		widget->setMinimumSize(widget->width(), widget->height());
		widget->setMaximumSize(widget->width(), widget->height());
		win->adjustSize();
		win->setFixedSize(win->width(), win->height());
	} else {
		win->resize(widget->width(),
		            widget->height() + win->menuBar()->height());
	}

	Timer* timer = new Timer(jalv);
	timer->start(1000 / jalv->ui_update_hz);

	init_cli_thread(jalv);
	int ret = app->exec();
	zix_sem_post(&jalv->done);
	return ret;
}

int
jalv_close_ui(Jalv*)
{
	app->quit();
	return 0;
}

//************************************************************

static void
jalv_print_controls(Jalv* jalv, bool writable, bool readable)
{
	for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
		ControlID* const control = jalv->controls.controls[i];
		if ((control->is_writable && writable) ||
		    (control->is_readable && readable)) {
			struct Port* const port = &jalv->ports[control->index];
			printf("%s = %f\n",
			       lilv_node_as_string(control->symbol),
			       port->control);
		}
	}
}

static int
jalv_print_preset(Jalv*           ZIX_UNUSED(jalv),
                  const LilvNode* node,
                  const LilvNode* title,
                  void*           ZIX_UNUSED(data))
{
	printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
	return 0;
}

static void
jalv_process_command(Jalv* jalv, const char* cmd)
{
	char     sym[1024];
	char     sym2[1024];
	uint32_t index = 0;
	float    value = 0.0f;
	int      count;
	if (!strncmp(cmd, "help", 4)) {
		fprintf(stderr,
		        "Commands:\n"
		        "  help              Display this help message\n"
		        "  controls          Print settable control values\n"
		        "  monitors          Print output control values\n"
		        "  presets           Print available presets\n"
		        "  preset URI        Set preset\n"
		        "  save preset [BANK_URI,] LABEL\n"
		        "                    Save preset (BANK_URI is optional)\n"
		        "  set INDEX VALUE   Set control value by port index\n"
		        "  set SYMBOL VALUE  Set control value by symbol\n"
		        "  SYMBOL = VALUE    Set control value by symbol\n");
	} else if (strcmp(cmd, "presets\n") == 0) {
		jalv_unload_presets(jalv);
		jalv_load_presets(jalv, jalv_print_preset, NULL);
	} else if (sscanf(cmd, "preset %1023[-a-zA-Z0-9_:/.%%#]", sym) == 1) {
		LilvNode* preset = lilv_new_uri(jalv->world, sym);
		lilv_world_load_resource(jalv->world, preset);
		jalv_apply_preset(jalv, preset);
		set_window_title(jalv);
		lilv_node_free(preset);
		jalv_print_controls(jalv, true, false);
	} else if (sscanf(cmd, "save preset %1023[-a-zA-Z0-9_:/.%%#, ]", sym) == 1) {
		char dir_preset[1024];
		char fname_preset[1024];
		char *plugin_name = jalv_get_plugin_name(jalv);
		char *saveptr = sym;
		char *bank_uri = strtok_r(sym, ",", &saveptr);
		char *label_preset = strtok_r(NULL, ",", &saveptr);
		if (!label_preset) {
			label_preset = bank_uri;
			bank_uri = NULL;
		}
		sprintf(dir_preset, "%s/%s.presets.lv2", jalv->opts.preset_path, plugin_name);
		sprintf(fname_preset, "%s.ttl", label_preset);
		jalv_fix_filename(fname_preset);
		if (bank_uri) {
			jalv_save_bank_preset(jalv, dir_preset, bank_uri, NULL, label_preset, fname_preset);
		} else {
			jalv_save_preset(jalv, dir_preset, NULL, label_preset, fname_preset);
		}
		free(plugin_name);
	} else if (strcmp(cmd, "controls\n") == 0) {
		jalv_print_controls(jalv, true, false);
	} else if (strcmp(cmd, "monitors\n") == 0) {
		jalv_print_controls(jalv, false, true);
	} else if (sscanf(cmd, "set %u %f", &index, &value) == 2) {
		if (index < jalv->num_ports) {
			jalv->ports[index].control = value;
			jalv_print_control(jalv, &jalv->ports[index], value);
		} else {
			fprintf(stderr, "error: port index out of range\n");
		}
	} else if (sscanf(cmd, "set %1023[a-zA-Z0-9_] %f", sym, &value) == 2 ||
	           sscanf(cmd, "%1023[a-zA-Z0-9_] = %f", sym, &value) == 2) {
		struct Port* port = NULL;
		for (uint32_t i = 0; i < jalv->num_ports; ++i) {
			struct Port* p = &jalv->ports[i];
			const LilvNode* s = lilv_port_get_symbol(jalv->plugin, p->lilv_port);
			if (!strcmp(lilv_node_as_string(s), sym)) {
				port = p;
				break;
			}
		}
		if (port) {
			port->control = value;
			jalv_print_control(jalv, port, value);
		} else {
			fprintf(stderr, "error: no control named `%s'\n", sym);
		}
	} else {
		fprintf(stderr, "error: invalid command (try `help')\n");
	}
}


void * cli_thread(void *arg) {
	while (1) {
		char line[1024];
		printf("> ");
		if (fgets(line, sizeof(line), stdin)) {
			jalv_process_command((Jalv*) arg, line);
		} else {
			break;
		}
	}
	return NULL;
}

pthread_t init_cli_thread(Jalv* jalv) {
	//Drop stderr output
	stderr = fopen("/dev/null", "w");

	pthread_t tid;
	int err=pthread_create(&tid, NULL, &cli_thread, jalv);
	if (err != 0) {
		printf("Can't create CLI thread :[%s]", strerror(err));
		return 0;
	} else {
		printf("CLI thread created successfully\n");
		return tid;
	}
}

}  // extern "C"
