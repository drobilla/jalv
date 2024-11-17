// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_MAPPER_H
#define JALV_MAPPER_H

#include "attributes.h"

#include "lv2/urid/urid.h"
#include "zix/attributes.h"

// URI to URID mapping and unmapping
JALV_BEGIN_DECLS

/// Opaque URI mapper implementation
typedef struct JalvMapperImpl JalvMapper;

/// Allocate, configure, and return a new URI mapper
JalvMapper*
jalv_mapper_new(void);

/// Free memory allocated by jalv_mapper_new()
void
jalv_mapper_free(JalvMapper* mapper);

/// Return a pointer to the mapper's LV2 URID map
LV2_URID_Map*
jalv_mapper_urid_map(JalvMapper* mapper);

/// Return a poitner to the mapper's LV2 URID unmap
LV2_URID_Unmap*
jalv_mapper_urid_unmap(JalvMapper* mapper);

/// Map a URI string to a URID
LV2_URID
jalv_mapper_map_uri(JalvMapper* mapper, const char* sym);

/// Unmap a URID back to a URI string if possible, or return NULL
ZIX_PURE_FUNC const char*
jalv_mapper_unmap_uri(const JalvMapper* mapper, LV2_URID id);

JALV_END_DECLS

#endif // JALV_MAPPER_H
