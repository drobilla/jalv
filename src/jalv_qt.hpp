// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "state.h"
#include "types.h"

#include "lilv/lilv.h"

#include <QAction>
#include <QGroupBox>
#include <QObject>
#include <QString>
#include <QtCore>

#include <map>
#include <vector>

struct Port;

class QDial;
class QLabel;
class QWidget;

class PresetAction : public QAction
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
  Jalv* jalv;
  Port* port;
};

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
  Port*             port;

  QLabel* label;
  QString name;
  int     steps;
  float   max{1.0f};
  float   min{0.0f};
  bool    isInteger{};
  bool    isEnum{};
  bool    isLogarithmic{};

  std::vector<float>           scalePoints;
  std::map<float, const char*> scaleMap;
};
