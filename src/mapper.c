// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "mapper.h"

#include "symap.h"

#include "lv2/urid/urid.h"
#include "zix/sem.h"

#include <stdint.h>
#include <stdlib.h>

struct JalvMapperImpl {
  Symap*         symap;
  ZixSem         lock;
  LV2_URID_Map   map;
  LV2_URID_Unmap unmap;
};

static LV2_URID
map_uri(LV2_URID_Map_Handle handle, const char* const uri)
{
  JalvMapper* const mapper = (JalvMapper*)handle;
  zix_sem_wait(&mapper->lock);

  const LV2_URID id = symap_map(mapper->symap, uri);

  zix_sem_post(&mapper->lock);
  return id;
}

static const char*
unmap_uri(LV2_URID_Unmap_Handle handle, const LV2_URID urid)
{
  JalvMapper* const mapper = (JalvMapper*)handle;
  zix_sem_wait(&mapper->lock);

  const char* const uri = symap_unmap(mapper->symap, urid);

  zix_sem_post(&mapper->lock);
  return uri;
}

JalvMapper*
jalv_mapper_new(void)
{
  JalvMapper* const mapper = (JalvMapper*)calloc(1, sizeof(JalvMapper));
  if (mapper) {
    mapper->symap        = symap_new();
    mapper->map.handle   = mapper;
    mapper->map.map      = map_uri;
    mapper->unmap.handle = mapper;
    mapper->unmap.unmap  = unmap_uri;
    zix_sem_init(&mapper->lock, 1);
  }
  return mapper;
}

void
jalv_mapper_free(JalvMapper* const mapper)
{
  if (mapper) {
    zix_sem_destroy(&mapper->lock);
    symap_free(mapper->symap);
    free(mapper);
  }
}

LV2_URID_Map*
jalv_mapper_urid_map(JalvMapper* const mapper)
{
  return &mapper->map;
}

LV2_URID_Unmap*
jalv_mapper_urid_unmap(JalvMapper* const mapper)
{
  return &mapper->unmap;
}

LV2_URID
jalv_mapper_map_uri(JalvMapper* const mapper, const char* const sym)
{
  return symap_map(mapper->symap, sym);
}

const char*
jalv_mapper_unmap_uri(const JalvMapper* mapper, uint32_t id)
{
  return symap_unmap(mapper->symap, id);
}
