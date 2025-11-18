// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "../port.h"
#include "../state.h"
#include "../types.h"

#include <lilv/lilv.h>

#include <QAction>
#include <QGroupBox>
#include <QObject>
#include <QString>
#include <QtCore>

#include <map>
#include <vector>

class QDial;
class QLabel;
class QWidget;

class PresetAction final : public QAction
{
  Q_OBJECT // NOLINT

public:
  PresetAction(QObject* parent, Jalv* jalv, LilvNode* preset)
    : QAction(parent)
    , _jalv(jalv)
    , _preset(preset)
  {
    connect(this, SIGNAL(triggered()), this, SLOT(presetChosen()));
  }

  Q_SLOT void presetChosen() { jalv_apply_preset(_jalv, _preset); }

private:
  Jalv*     _jalv;
  LilvNode* _preset;
};

struct PortContainer {
  Jalv*     jalv;
  JalvPort* port;
};

class Controller final : public QGroupBox
{
  Q_OBJECT // NOLINT

public:
  explicit Controller(PortContainer portContainer, QWidget* parent);

  Q_SLOT void dialChanged(int value);

  void setValue(float value);

private:
  void    setRange(float min, float max);
  QString getValueLabel(float value);
  float   getValue();
  int     stringWidth(const QString& str);

  QDial*    _dial;
  Jalv*     _jalv;
  JalvPort* _port;

  QLabel* _label;
  QString _name;
  int     _steps;
  float   _max{1.0f};
  float   _min{0.0f};
  bool    _isInteger{};
  bool    _isEnum{};
  bool    _isLogarithmic{};

  std::vector<float>           _scalePoints;
  std::map<float, const char*> _scaleMap;
};
