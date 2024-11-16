// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "jalv_qt.hpp"
#include "frontend.h"
#include "jalv_internal.h"
#include "nodes.h"
#include "options.h"
#include "port.h"

#include "lilv/lilv.h"
#include "suil/suil.h"
#include "zix/sem.h"

#include <QAction>
#include <QApplication>
#include <QDial>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QList>
#include <QMainWindow>
#include <QMargins>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtCore>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

constexpr int CONTROL_WIDTH = 150;
constexpr int DIAL_STEPS    = 10000;

namespace {

QApplication* app = nullptr;

class FlowLayout final : public QLayout
{
public:
  explicit FlowLayout(int margin, int hSpacing, int vSpacing);

  FlowLayout(const FlowLayout&)            = delete;
  FlowLayout& operator=(const FlowLayout&) = delete;

  FlowLayout(FlowLayout&&)             = delete;
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
  void             setGeometry(const QRect& rect) override;
  QSize            sizeHint() const override;
  QLayoutItem*     takeAt(int index) override;

private:
  int doLayout(const QRect& rect, bool testOnly) const;
  int smartSpacing(QStyle::PixelMetric pm) const;

  QList<QLayoutItem*> _itemList;
  int                 _hSpace;
  int                 _vSpace;
};

FlowLayout::FlowLayout(int margin, int hSpacing, int vSpacing)
  : _hSpace(hSpacing)
  , _vSpace(vSpacing)
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
  _itemList.append(item);
}

int
FlowLayout::horizontalSpacing() const
{
  if (_hSpace >= 0) {
    return _hSpace;
  }

  return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int
FlowLayout::verticalSpacing() const
{
  if (_vSpace >= 0) {
    return _vSpace;
  }

  return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int
FlowLayout::count() const
{
  return _itemList.size();
}

QLayoutItem*
FlowLayout::itemAt(int index) const
{
  return _itemList.value(index);
}

QLayoutItem*
FlowLayout::takeAt(int index)
{
  if (index >= 0 && index < _itemList.size()) {
    return _itemList.takeAt(index);
  }

  return nullptr;
}

Qt::Orientations
FlowLayout::expandingDirections() const
{
  return {};
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
FlowLayout::setGeometry(const QRect& rect)
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
  QSize size = {};
  for (QLayoutItem* const item : _itemList) {
    size = size.expandedTo(item->minimumSize());
  }

  const auto m = contentsMargins();
  return size + QSize{m.left() + m.right(), m.top() + m.bottom()};
}

int
FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
  int left   = 0;
  int top    = 0;
  int right  = 0;
  int bottom = 0;
  getContentsMargins(&left, &top, &right, &bottom);

  const QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
  int         x             = effectiveRect.x();
  int         y             = effectiveRect.y();
  int         lineHeight    = 0;

  for (QLayoutItem* const item : _itemList) {
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
  QObject* const parent = this->parent();
  if (!parent) {
    return -1;
  }

  auto* const parentWidget = qobject_cast<QWidget*>(parent);
  if (parentWidget) {
    return parentWidget->style()->pixelMetric(pm, nullptr, parentWidget);
  }

  auto* const parentLayout = qobject_cast<QLayout*>(parent);
  if (parentLayout) {
    return parentLayout->spacing();
  }

  return -1;
}

class Timer : public QTimer
{
public:
  explicit Timer(Jalv* jalv)
    : _jalv(jalv)
  {}

  void timerEvent(QTimerEvent*) override { jalv_update(_jalv); }

private:
  Jalv* _jalv;
};

int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
  auto* const menu  = static_cast<QMenu*>(data);
  const char* label = lilv_node_as_string(title);

  QAction* action = new PresetAction(menu, jalv, lilv_node_duplicate(node));
  action->setText(label);
  menu->addAction(action);
  return 0;
}

} // namespace

Control::Control(PortContainer portContainer, QWidget* parent)
  : QGroupBox(parent)
  , _dial(new QDial())
  , _plugin(portContainer.jalv->plugin)
  , _port(portContainer.port)
  , _label(new QLabel())
{
  const JalvNodes*  nodes    = &portContainer.jalv->nodes;
  const LilvPlugin* plugin   = _plugin;
  const LilvPort*   lilvPort = _port->lilv_port;

  LilvNode* nmin = nullptr;
  LilvNode* nmax = nullptr;
  LilvNode* ndef = nullptr;
  lilv_port_get_range(plugin, lilvPort, &ndef, &nmin, &nmax);

  LilvNode* stepsNode =
    lilv_port_get(plugin, lilvPort, nodes->pprops_rangeSteps);
  if (lilv_node_is_int(stepsNode)) {
    _steps = std::max(lilv_node_as_int(stepsNode), 2);
  } else {
    _steps = DIAL_STEPS;
  }
  lilv_node_free(stepsNode);

  // Fill scalePoints Map
  LilvScalePoints* sp = lilv_port_get_scale_points(plugin, lilvPort);
  if (sp) {
    LILV_FOREACH (scale_points, s, sp) {
      const LilvScalePoint* p   = lilv_scale_points_get(sp, s);
      const LilvNode*       val = lilv_scale_point_get_value(p);
      if (!lilv_node_is_float(val) && !lilv_node_is_int(val)) {
        continue;
      }

      const float f = lilv_node_as_float(val);
      _scalePoints.push_back(f);
      _scaleMap[f] = lilv_node_as_string(lilv_scale_point_get_label(p));
    }

    lilv_scale_points_free(sp);
  }

  // Check port properties
  _isLogarithmic =
    lilv_port_has_property(plugin, lilvPort, nodes->pprops_logarithmic);
  _isInteger = lilv_port_has_property(plugin, lilvPort, nodes->lv2_integer);
  _isEnum    = lilv_port_has_property(plugin, lilvPort, nodes->lv2_enumeration);

  if (lilv_port_has_property(plugin, lilvPort, nodes->lv2_toggled)) {
    _isInteger = true;

    if (!_scaleMap[0]) {
      _scaleMap[0] = "Off";
    }
    if (!_scaleMap[1]) {
      _scaleMap[1] = "On";
    }
  }

  // Find and set min, max and default values for port
  const float defaultValue = ndef ? lilv_node_as_float(ndef) : _port->control;
  setRange(lilv_node_as_float(nmin), lilv_node_as_float(nmax));
  setValue(defaultValue);

  // Fill layout
  auto* const layout = new QVBoxLayout();
  layout->addWidget(_label, 0, Qt::AlignHCenter);
  layout->addWidget(_dial, 0, Qt::AlignHCenter);
  setLayout(layout);

  setMinimumWidth(CONTROL_WIDTH);
  setMaximumWidth(CONTROL_WIDTH);

  LilvNode* nname = lilv_port_get_name(plugin, lilvPort);
  _name           = QString("%1").arg(lilv_node_as_string(nname));

  // Handle long names
  if (stringWidth(_name) > CONTROL_WIDTH) {
    setTitle(fontMetrics().elidedText(_name, Qt::ElideRight, CONTROL_WIDTH));
  } else {
    setTitle(_name);
  }

  // Set tooltip if comment is available
  LilvNode* comment = lilv_port_get(plugin, lilvPort, nodes->rdfs_comment);
  if (comment) {
    auto* const tooltip = new QString();
    tooltip->append(lilv_node_as_string(comment));
    setToolTip(*tooltip);
  }

  setFlat(true);

  connect(_dial, SIGNAL(valueChanged(int)), this, SLOT(dialChanged(int)));

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

  if (_isInteger) {
    step = value;
  } else if (_isEnum) {
    step = (std::find(_scalePoints.begin(), _scalePoints.end(), value) -
            _scalePoints.begin());
  } else if (_isLogarithmic) {
    step = _steps * logf(value / _min) / logf(_max / _min);
  } else {
    step = value * _steps;
  }

  _dial->setValue(step);
  _label->setText(getValueLabel(value));
}

QString
Control::getValueLabel(float value)
{
  if (_scaleMap[value]) {
    if (stringWidth(_scaleMap[value]) > CONTROL_WIDTH) {
      _label->setToolTip(_scaleMap[value]);
      return fontMetrics().elidedText(
        QString(_scaleMap[value]), Qt::ElideRight, CONTROL_WIDTH);
    }
    return _scaleMap[value];
  }

  return QString("%1").arg(value);
}

void
Control::setRange(float minRange, float maxRange)
{
  _min = minRange;
  _max = maxRange;

  if (_isLogarithmic) {
    minRange = 1;
    maxRange = _steps;
  } else if (_isEnum) {
    minRange = 0;
    maxRange = _scalePoints.size() - 1;
  } else if (!_isInteger) {
    minRange *= _steps;
    maxRange *= _steps;
  }

  _dial->setRange(minRange, maxRange);
}

float
Control::getValue()
{
  if (_isEnum) {
    return _scalePoints[_dial->value()];
  }

  if (_isInteger) {
    return _dial->value();
  }

  if (_isLogarithmic) {
    return _min *
           powf(_max / _min, static_cast<float>(_dial->value()) / (_steps - 1));
  }

  return static_cast<float>(_dial->value()) / _steps;
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
  const float value = getValue();

  _label->setText(getValueLabel(value));
  _port->control = value;
}

namespace {

bool
portGroupLessThan(const PortContainer& p1, const PortContainer& p2)
{
  const Jalv*     jalv  = p1.jalv;
  const LilvPort* port1 = p1.port->lilv_port;
  const LilvPort* port2 = p2.port->lilv_port;

  LilvNode* group1 = lilv_port_get(jalv->plugin, port1, jalv->nodes.pg_group);
  LilvNode* group2 = lilv_port_get(jalv->plugin, port2, jalv->nodes.pg_group);

  const int cmp = (group1 && group2) ? strcmp(lilv_node_as_string(group1),
                                              lilv_node_as_string(group2))
                                     : (reinterpret_cast<intptr_t>(group1) -
                                        reinterpret_cast<intptr_t>(group2));

  lilv_node_free(group2);
  lilv_node_free(group1);

  return cmp < 0;
}

QWidget*
build_control_widget(Jalv* jalv)
{
  const LilvPlugin* plugin = jalv->plugin;
  LilvWorld*        world  = jalv->world;

  QList<PortContainer> portContainers;
  for (unsigned i = 0; i < jalv->num_ports; ++i) {
    if (!jalv->opts.show_hidden &&
        lilv_port_has_property(
          plugin, jalv->ports[i].lilv_port, jalv->nodes.pprops_notOnGUI)) {
      continue;
    }

    if (jalv->ports[i].type == TYPE_CONTROL) {
      portContainers.append(PortContainer{jalv, &jalv->ports[i]});
    }
  }

  std::sort(portContainers.begin(), portContainers.end(), portGroupLessThan);

  auto* const grid       = new QWidget();
  auto* const flowLayout = new FlowLayout(-1, -1, -1);
  QLayout*    layout     = flowLayout;

  LilvNode*    lastGroup   = nullptr;
  QHBoxLayout* groupLayout = nullptr;
  for (int i = 0; i < portContainers.count(); ++i) {
    const PortContainer   portContainer = portContainers[i];
    const JalvPort* const port          = portContainer.port;

    auto* const control = new Control(portContainer, nullptr);
    LilvNode*   group =
      lilv_port_get(plugin, port->lilv_port, jalv->nodes.pg_group);
    if (group) {
      if (!groupLayout || !lilv_node_equals(group, lastGroup)) {
        // Group has changed
        const LilvNode* groupName =
          lilv_world_get(world, group, jalv->nodes.lv2_name, nullptr);
        if (!groupName) {
          groupName =
            lilv_world_get(world, group, jalv->nodes.rdfs_label, nullptr);
        }

        auto* const groupBox = new QGroupBox(lilv_node_as_string(groupName));

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

    const uint32_t index      = lilv_port_get_index(plugin, port->lilv_port);
    jalv->ports[index].widget = control;
  }
  lilv_node_free(lastGroup);

  grid->setLayout(layout);

  return grid;
}

} // namespace

extern "C" {

int
jalv_frontend_init(JalvFrontendArgs* const args, JalvOptions*)
{
  app = new QApplication(*args->argc, *args->argv, true);
  app->setStyleSheet("QGroupBox::title { subcontrol-position: top center }");
  --*args->argc;
  ++*args->argv;
  return 0;
}

const char*
jalv_frontend_ui_type(void)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  return "http://lv2plug.in/ns/extensions/ui#Qt5UI";
#else
  return "http://lv2plug.in/ns/extensions/ui#Qt6UI";
#endif
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
  } else {
    auto* const control = static_cast<Control*>(jalv->ports[port_index].widget);
    if (control) {
      control->setValue(*static_cast<const float*>(buffer));
    }
  }
}

bool
jalv_frontend_discover(const Jalv*)
{
  return true;
}

float
jalv_frontend_refresh_rate(const Jalv*)
{
  return static_cast<float>(QGuiApplication::primaryScreen()->refreshRate());
}

float
jalv_frontend_scale_factor(const Jalv*)
{
  return static_cast<float>(
    QGuiApplication::primaryScreen()->devicePixelRatio());
}

LilvNode*
jalv_frontend_select_plugin(Jalv*)
{
  return nullptr;
}

int
jalv_frontend_open(Jalv* jalv)
{
  auto* const win          = new QMainWindow();
  QMenu*      file_menu    = win->menuBar()->addMenu("&File");
  QMenu*      presets_menu = win->menuBar()->addMenu("&Presets");
  auto* const quit_action  = new QAction("&Quit", win);

  QObject::connect(quit_action, SIGNAL(triggered()), win, SLOT(close()));
  quit_action->setShortcuts(QKeySequence::Quit);
  quit_action->setStatusTip("Quit Jalv");
  file_menu->addAction(quit_action);

  jalv_load_presets(jalv, add_preset_to_menu, presets_menu);

  if (jalv->ui && !jalv->opts.generic_ui) {
    jalv_ui_instantiate(jalv, jalv_frontend_ui_type(), win);
  }

  QWidget* widget = nullptr;
  if (jalv->ui_instance) {
    widget = static_cast<QWidget*>(suil_instance_get_widget(jalv->ui_instance));
  } else {
    auto* const controlWidget = build_control_widget(jalv);
    auto* const scrollArea    = new QScrollArea();
    scrollArea->setWidget(controlWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumWidth(800);
    scrollArea->setMinimumHeight(600);
    widget = scrollArea;
  }

  LilvNode* name = lilv_plugin_get_name(jalv->plugin);
  win->setWindowTitle(lilv_node_as_string(name));
  lilv_node_free(name);

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
    win->resize(widget->width(), widget->height() + win->menuBar()->height());
  }

  auto* const timer = new Timer(jalv);
  timer->start(1000 / jalv->ui_update_hz);

  const int ret = app->exec();
  zix_sem_post(&jalv->done);
  return ret;
}

int
jalv_frontend_close(Jalv*)
{
  app->quit();
  return 0;
}

} // extern "C"
