/*
  Copyright 2017 Timo Wischer <twischer@de.adit-jv.com>

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

#ifndef CONTROL_API_H
#define CONTROL_API_H

/**
 * @addtogroup Internal
 * @{
 * @name Control API
 * @{
 * 
 * @brief The following functions are used to handle the API.
 * These functions are only used internally
 */

#include "jalv.h"
#include "jalv_internal.h"

/**
 * @brief api_ctl_init creates and initilizes all required IPC mechanismen
 * @param jalv
 * @return <0 on error
 */
int jalv_api_ctl_init(Jalv* const jalv);

/**
 * @brief api_ctl_lock has to be called before the control port data will be changed
 * @param jalv
 * @return <0 on error
 * This function will be called from real time context
 */
int jalv_api_ctl_lock(Jalv* const jalv, const bool process_requests);

/**
 * @brief api_ctl_unlock has to be called to provide the changed control data to the IPC clients
 * @param jalv
 * @return <0 on error
 */
int jalv_api_ctl_unlock(Jalv* const jalv, const bool trigger_events);

/**
 * @brief jalv_api_ctl_destroy frees all internal ressources of the JALV API
 * @param jalv
 */
void jalv_api_ctl_destroy(Jalv* const jalv);

/**
   @endcond
   @}
   @}
*/

#endif // CONTROL_API_H
