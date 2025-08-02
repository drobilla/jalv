// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "dumper.h"

#include "log.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <serd/serd.h>
#include <sratom/sratom.h>

#include <stdint.h>
#include <stdlib.h>

struct JalvDumperImpl {
  LV2_URID_Unmap* unmap;
  SerdEnv*        env;
  Sratom*         sratom;
};

JalvDumper*
jalv_dumper_new(LV2_URID_Map* const map, LV2_URID_Unmap* const unmap)
{
  JalvDumper* const dumper = (JalvDumper*)calloc(1, sizeof(JalvDumper));
  SerdEnv* const    env    = serd_env_new(NULL);
  Sratom* const     sratom = sratom_new(map);
  if (!dumper || !env || !sratom) {
    jalv_dumper_free(dumper);
    return NULL;
  }

  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"patch", (const uint8_t*)LV2_PATCH_PREFIX);
  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"time", (const uint8_t*)LV2_TIME_PREFIX);
  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"xsd", (const uint8_t*)LILV_NS_XSD);

  sratom_set_env(sratom, env);

  dumper->env    = env;
  dumper->sratom = sratom;
  dumper->unmap  = unmap;
  return dumper;
}

void
jalv_dumper_free(JalvDumper* const dumper)
{
  if (dumper) {
    sratom_free(dumper->sratom);
    serd_env_free(dumper->env);
    free(dumper);
  }
}

void
jalv_dump_atom(JalvDumper* const     dumper,
               FILE* const           stream,
               const char* const     label,
               const LV2_Atom* const atom,
               const int             color)
{
  if (dumper) {
    char* const str = sratom_to_turtle(dumper->sratom,
                                       dumper->unmap,
                                       "jalv:",
                                       NULL,
                                       NULL,
                                       atom->type,
                                       atom->size,
                                       LV2_ATOM_BODY_CONST(atom));

    jalv_ansi_start(stream, color);
    fprintf(stream, "\n# %s (%u bytes):\n%s\n", label, atom->size, str);
    jalv_ansi_reset(stream);
    free(str);
  }
}
