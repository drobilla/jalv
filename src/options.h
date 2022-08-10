// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_OPTIONS_H
#define JALV_OPTIONS_H

#include "attributes.h"

#include <stdint.h>

JALV_BEGIN_DECLS

typedef struct {
  char*    name;            ///< Client name
  int      name_exact;      ///< Exit if name is taken
  char*    load;            ///< Path for state to load
  char*    preset;          ///< URI of preset to load
  char**   controls;        ///< Control values
  uint32_t buffer_size;     ///< Plugin <= >UI communication buffer size
  double   update_rate;     ///< UI update rate in Hz
  double   scale_factor;    ///< UI scale factor
  int      dump;            ///< Dump communication iff true
  int      trace;           ///< Print trace log iff true
  int      generic_ui;      ///< Use generic UI iff true
  int      show_hidden;     ///< Show controls for notOnGUI ports
  int      no_menu;         ///< Hide menu iff true
  int      show_ui;         ///< Show non-embedded UI
  int      print_controls;  ///< Print control changes to stdout
  int      non_interactive; ///< Do not listen for commands on stdin
  char*    ui_uri;          ///< URI of UI to load
} JalvOptions;

JALV_END_DECLS

#endif // JALV_OPTIONS_H
