/*
  Copyright 2019 Timo Wischer <twischer@de.adit-jv.com>

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

#ifndef CONTROL_BACKEND_H
#define CONTROL_BACKEND_H
#include "jalv_internal.h"

/**
 * @brief jalv_control_backend_init initializes the ControlBackend structure if
 * required. This is called once for each JALV instance.
 * @param jalv
 * @return <0 on error
 *          0 on success
 */
int jalv_control_backend_init(Jalv* const jalv);

/**
 * @brief jalv_control_port_init initializes the ControlPort structure. This is
 * called for each control port.
 * @param jalv
 * @param port
 * @return <0 on error
 *          0 on success
 */
int jalv_control_port_init(Jalv* const jalv, struct Port* const port);

/**
 * @brief jalv_control_port_destroy destroys the ControlPort structure. This is
 * called for each control port.
 * @param jalv
 * @param port
 * @return
 */
int jalv_control_port_destroy(Jalv *jalv, struct Port* const port);

/**
 * @brief jalv_control_backend_destroy destroys the ControlBackend structure if
 * required. This is called once for each JALV instance.
 * @param jalv
 * @return
 */
int jalv_control_backend_destroy(Jalv* const jalv);

/**
 * @brief jalv_control_lock has to be called before accessing returned pointer
 * of jalv_control_data()
 * @param jalv
 * @return <0 on error
 *          0 on success
 */
int jalv_control_lock(Jalv* const jalv);

/**
 * @brief jalv_control_before_run called with jalv_control_lock() before calling
 * the next lv2::run()
 * @param jalv
 * @return <0 on error
 *          0 on success
 */
int jalv_control_before_run(Jalv* const jalv);

/**
 * @brief jalv_control_after_run called with jalv_control_lock() after calling
 * the next lv2::run()
 * @param jalv
 * @return <0 on error
 *          0 on success
 */
int jalv_control_after_run(Jalv* const jalv);

/**
 * @brief jalv_control_unlock unlocks the control memory. Therefore pointer
 * returned by jalv_control_data() should not be accessed anymore.
 * @param jalv
 * @return <0 on error
 *          0 on success
 */
int jalv_control_unlock(Jalv* const jalv);

/**
 * @brief jalv_control_data has only to be used when jalv_control_lock() was
 * called before
 * @param jalv
 * @param port
 * @return NULL on error else pointer to memory
 */
float* const jalv_control_data(Jalv* const jalv, struct Port* const port);

/**
 * @brief jalv_control_get can be called from any thread context. No need to
 * call jalv_control_lock() before
 * @param jalv
 * @param port
 * @return the current value of the port
 */
float jalv_control_get(Jalv* const jalv, struct Port* const port);

/**
 * @brief jalv_control_set can be called from any thread context. No need to
 * call jalv_control_lock() before
 * @param jalv
 * @param port
 * @param value the new value for the port
 * @return <0 on error
 *          0 on success
 */
int jalv_control_set(Jalv* const jalv, struct Port* const port, const float value);

#endif // CONTROL_BACKEND_H
