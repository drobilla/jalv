// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_DUMPER_H
#define JALV_DUMPER_H

#include "attributes.h"

#include <lv2/atom/atom.h>
#include <lv2/urid/urid.h>

#include <stdio.h>

// LV2 atom dumper
JALV_BEGIN_DECLS

/// Dumper for writing atoms as Turtle for debugging
typedef struct JalvDumperImpl JalvDumper;

/// Allocate, configure, and return a new atom dumper
JalvDumper*
jalv_dumper_new(LV2_URID_Map* map, LV2_URID_Unmap* unmap);

/// Free memory allocated by jalv_init_dumper()
void
jalv_dumper_free(JalvDumper* dumper);

/// Dump an atom to stdout
void
jalv_dump_atom(JalvDumper*     dumper,
               FILE*           stream,
               const char*     label,
               const LV2_Atom* atom,
               int             color);

JALV_END_DECLS

#endif // JALV_DUMPER_H
