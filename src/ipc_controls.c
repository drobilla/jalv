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

#include <stdbool.h>
#include <math.h>
#include "zix/atomic.h"
#include "ipc_controls.h"

typedef struct {
	atomic_bool blocking_active;
	uint32_t blocking_start_cycle_id;
} jalv_shm_t;


static size_t lv2_get_port_size(const struct Port* const port)
{
	/* default data type is float*/
	return (port->buf_size > 0) ? port->buf_size : sizeof(float);
}


static float lv2_get_value(const LilvNode* const value, const char* const name)
{
	if (lilv_node_is_float(value) || lilv_node_is_int(value)) {
		return lilv_node_as_float(value);
	} else if (lilv_node_is_bool(value)) {
		return lilv_node_as_bool(value) ? 1.0 : 0.0;
	} else {
		ERR("Data type of Lv2 control port %s not supported!\n", name);
		return 0.0f;
	}
}

static unsigned int jalv_api_ctl_init_ports(Jalv* const jalv, const bool writable, const unsigned int index_offset, lv2_ctl_io_t* const ctl_io,
											size_t* const data_offset, size_t* const scale_point_offset)
{
	lv2_ctl_io_data_t* const ctls = ctl_io->ctls;

	unsigned int ctl_index = index_offset;
	for (uint32_t i = 0; i < jalv->controls.n_controls; ++i) {
		const ControlID* const control = jalv->controls.controls[i];
		if (control->type != PORT) {
			continue;
		}
		if (control->is_writable != writable) {
			continue;
		}

		lv2_ctl_io_data_t* const ctl_data = &ctls[ctl_index];

		const char* const symbol = lilv_node_as_string(control->symbol);
		strncpy(ctl_data->name, symbol, sizeof(ctl_data->name));

		struct Port* const port = &jalv->ports[control->index];
		if (control->is_enumeration) {
			ctl_data->type = JALV_API_PORT_TYPE_ENUMERATION;
		} else if (control->is_toggle) {
			ctl_data->type = JALV_API_PORT_TYPE_TOGGLE;
		} else if (control->is_integer) {
			ctl_data->type = JALV_API_PORT_TYPE_INTEGER;
		} else {
			/* if the Lv2 minimumSize attribute was set for this port
			 * but no additional port type properties were set,
			 * it is not clear how to interpret the port data.
			 * Therefore the data will be interpreted as byte array and
			 * the user has to care about the exact type.
			 */
			ctl_data->type = ( (port->buf_size <= 0) ? JALV_API_PORT_TYPE_DEFAULT : JALV_API_PORT_TYPE_BYTES );
		}

		ctl_data->min = (control->min) ? lv2_get_value(control->min, ctl_data->name) : 0.0;
		ctl_data->max = (control->max) ? lv2_get_value(control->max, ctl_data->name) : 1.0;

		ctl_data->data_offset = *data_offset;

		port->control.shm_index = ctl_index;
		ctl_data->data_length = lv2_get_port_size(port);

		/* align memory always on sizeof(float) boundary.
		 * the data_offset has than always be used for accessing
		 */
		(*data_offset) += zix_round_up(ctl_data->data_length,
		                               sizeof(port->control.data[0]));


		/* assign a control data pointer to the shared memory */
		port->control.data = jalv_api_ctl_port_data_pointer(ctl_io, ctl_index);

		if (control->def) {
			/* initalize with default value */
			const float default_value = lv2_get_value(control->def, ctl_data->name);
			for (unsigned int k=0; k<(port->buf_size/sizeof(default_value)); k++) {
				port->control.data[k] = default_value;
			}
		} else {
			/* already set to zero when creating shared memory */
		}

		/* copy scale point information to shared memory if required */
		if (ctl_data->type == JALV_API_PORT_TYPE_ENUMERATION) {
			ctl_data->scale_points_offset = *scale_point_offset;

			const unsigned int scale_points_count = control->n_points;
			if (scale_points_count < 2) {
				ERR("Only %u scale points configured for control port '%s'. Looks not meaningful. "
						"Possibly a missconfiguration of the Lv2 plug-in!\n", scale_points_count, symbol);
			}

			ctl_data->scale_points_count = scale_points_count;
			(*scale_point_offset) += scale_points_count * sizeof(lv2_ctl_scale_point_t);

			for (unsigned int k=0; k<scale_points_count; k++) {
				const ScalePoint* const src_scale_point = &control->points[k];
				lv2_ctl_scale_point_t* const dest_scale_point = jalv_api_ctl_port_scale_point(ctl_io, ctl_index, k);

				dest_scale_point->value = src_scale_point->value;
				strncpy(dest_scale_point->label, src_scale_point->label, sizeof(dest_scale_point->label));
				/* always append terminating zero for the string */
				dest_scale_point->label[sizeof(dest_scale_point->label) - 1] = 0;
			}
		}

		ctl_index++;
	}

	return ctl_index;
}


static void* jalv_api_ctl_data_pointer(lv2_ctl_io_t * const ctl_io)
{
	return jalv_api_ctl_port_data_pointer(ctl_io, 0);
}


static int jalv_api_ctl_blocking_init(Jalv* const jalv, const size_t ctl_input_data_size)
{
	jalv->jalv_api_blocking.ctl_input_data = malloc(ctl_input_data_size);
	if (!jalv->jalv_api_blocking.ctl_input_data) {
		ERR("malloc(%lu) for commit feature failed.\n", ctl_input_data_size);
		return -2;
	}
	jalv->jalv_api_blocking.ctl_input_data_size = ctl_input_data_size;


	const char shm_name[] = JALV_API_SHM_PREFIX "cycle-id";
	ZixStatus ret = zix_shm_init(&jalv->jalv_api_blocking.shm, shm_name,
	                                   ZIX_MODE_CREATE | ZIX_MODE_RDWR,
	                                   sizeof(jalv_shm_t),
	                                   jalv->jalv_api.user_group, false);
	/* Shared memory was already created by
	 * another JALV instance.
	 * Try to open the existing one.
	 */
	if (ret == ZIX_STATUS_EXISTS) {
		ret = zix_shm_init(&jalv->jalv_api_blocking.shm, shm_name,
		                   ZIX_MODE_RDWR, 0, NULL, false);
	}
	if (ret != ZIX_STATUS_SUCCESS) {
		ERR("zix_shm_init(%s, %lu) failed\n", shm_name, sizeof(jalv_shm_t));
		return -1;
	}

	/* no need to initalize the shared memory here.
	 * It will be cleared with 0 by zix_shm_init()
	 */

	return 0;
}


int jalv_api_ctl_init(Jalv* const jalv)
{
	if (jalv->opts.user_group) {
		strncpy(jalv->jalv_api.user_group, jalv->opts.user_group, sizeof(jalv->jalv_api.user_group));
		jalv->jalv_api.user_group[sizeof(jalv->jalv_api.user_group) - 1] = 0;
	}

	/* find different size values which are required for
	 * the allocation of the API exchange memory
	 */
	unsigned int in_ctl_count = 0;
	unsigned int out_ctl_count = 0;
	size_t data_size = 0;
	size_t scale_point_count = 0;
	for (uint32_t i = 0; i < jalv->controls.n_controls; ++i) {
		const ControlID* const control = jalv->controls.controls[i];
		if (control->type == PORT) {
			if (control->is_writable) {
				in_ctl_count++;
			} else  {
				out_ctl_count++;
			}

			const size_t ctl_port_size = lv2_get_port_size(&jalv->ports[control->index]);
			data_size += zix_round_up(ctl_port_size,
			                          sizeof(jalv->ports[0].control.data[0]));
			scale_point_count += control->n_points;
		}
	}

	const size_t meta_size = (in_ctl_count + out_ctl_count) * sizeof(lv2_ctl_io_data_t);
	const size_t scale_point_size = scale_point_count * sizeof(lv2_ctl_scale_point_t);
	const size_t shm_size = sizeof(lv2_ctl_io_t) + meta_size + data_size + scale_point_size;


	char jalv_name[NAME_MAX];
	if (jalv_backend_instance_name(jalv, jalv_name, sizeof(jalv_name)) != 0) {
		return -1;
	}

	STRCAT(shm_name, JALV_API_SHM_PREFIX, jalv_name);
	if (zix_shm_init(&jalv->jalv_api.shm, shm_name, ZIX_MODE_CREATE | ZIX_MODE_RDWR, shm_size, jalv->jalv_api.user_group, true) != ZIX_STATUS_SUCCESS) {
		ERR("zix_shm_init(%s, %lu) failed\n", shm_name, shm_size);
		jalv_api_ctl_destroy(jalv);
		return -1;
	}

	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);
	ctl_io->in_ctl_count = in_ctl_count;
	ctl_io->out_ctl_count = out_ctl_count;

	/* the first control port data is located directly behind the last lv2_ctl_io_data_t structure */
	size_t data_offset = 0;
	size_t scale_point_offset = meta_size + data_size;
	jalv_api_ctl_init_ports(jalv, true,  0,                    ctl_io, &data_offset, &scale_point_offset);
	const size_t ctl_input_data_size = data_offset;
	jalv_api_ctl_init_ports(jalv, false, ctl_io->in_ctl_count, ctl_io, &data_offset, &scale_point_offset);

	/* check for overflow between the different sections */
	assert(ctl_count == LV2_CTL_COUNT(ctl_io));
	assert(data_offset <= data_size);
	assert( scale_point_offset <= (meta_size + data_size + scale_point_size) );


	STRCAT(req_name, JALV_API_REQ_PREFIX, jalv_name);
	/* there is no additional locking for the request queue required
	 * because all clients will wait for the unlock of the shm
	 * which will be done after initalizing the request queu
	 */
	if (zix_shq_init(&jalv->jalv_api.request_queue, req_name, ZIX_MODE_CREATE | ZIX_MODE_RDONLY | ZIX_MODE_NONBLOCKING, jalv->jalv_api.user_group) != ZIX_STATUS_SUCCESS) {
		ERR("zix_shq_init(%s) failed\n", req_name);
		jalv_api_ctl_destroy(jalv);
		return -1;
	}

	if (zix_shm_init_done(&jalv->jalv_api.shm) != ZIX_STATUS_SUCCESS) {
		ERR("zix_shm_init_done(%s) failed.\n", shm_name);
		jalv_api_ctl_destroy(jalv);
		return -1;
	}

	/* The event_queues will only be used if a JALV client subscribes for change events */
	for (unsigned int i=0; i<ARRAY_SIZE(jalv->jalv_api.events); i++) {
		zix_shq_clear(&jalv->jalv_api.events[i].event_queue);
	}

	if (jalv_api_ctl_blocking_init(jalv, ctl_input_data_size) != 0) {
		jalv_api_ctl_destroy(jalv);
		return -2;
	}

	jalv->prev_ctl_data = malloc(data_size);
	if (!jalv->prev_ctl_data) {
		ERR("malloc(%lu) failed.\n", data_size);
		jalv_api_ctl_destroy(jalv);
		return -2;
	}
	jalv->ctl_data_size = data_size;

	void* const prev_ctl_data = jalv->prev_ctl_data;
	void* const cur_ctl_data = jalv_api_ctl_data_pointer(ctl_io);
	memcpy(prev_ctl_data, cur_ctl_data, data_size);

	return 0;
}


static void jalv_api_request_drop(Jalv* const jalv,
                                  const jalv_api_request_t* const request,
                                  const size_t size)
{
	/* performe dummy read to keep the queue in sync */
	char values[size];
	if (zix_shq_read(&jalv->jalv_api.request_queue, values, sizeof(values)) != ZIX_STATUS_SUCCESS) {
		ERR("Data segment of change control value request from client ID %u could not be read. Possibly the JALV client sent a corrupted request.",
		    request->client_id);
	}
}


/**
 * @brief jalv_api_ctl_update_controls
 * @param jalv
 * @param request
 * @return  1 if the request was successfully processed
 * @return  0 if the request was ignored
 * @return <0 if an error occured
 */
static int jalv_api_ctl_update_controls(Jalv* const jalv, const jalv_api_request_t* const request)
{
	assert(request->type == JALV_API_CHANGE_VALUE);

	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);
	assert(ctl_io);

	/* only control input ports should be writeable by JALV API clients */
	if (request->ctl_index >= ctl_io->in_ctl_count) {
		const unsigned int count = LV2_CTL_COUNT(ctl_io);
		if (request->ctl_index >= count) {
			ERR("Control port index %u is out of range. (max %u)", request->ctl_index, count);
		} else {
			const char* const control_name = ctl_io->ctls[request->ctl_index].name;
			ERR("Output control port '%s' is read only and cannot be changed by JALV API clients. Ignoring request", control_name);
		}

		jalv_api_request_drop(jalv, request, request->data_size);
		return 0;
	}

	const lv2_ctl_io_data_t* const data = &ctl_io->ctls[request->ctl_index];

	size_t data_size = request->data_size;
	if (data_size > data->data_length) {
		ERR("Provided control data is bigger than the control port '%s' accepts. Ignoring request from client ID %u",
		    data->name, request->client_id);
		jalv_api_request_drop(jalv, request, request->data_size);
		return 0;
	}

	void* values = NULL;
	/* only use local buffer if blocking mode is active */
	if (ctl_io->blocking_active) {
		const size_t offset = ctl_io->ctls[request->ctl_index].data_offset;
		values = &jalv->jalv_api_blocking.ctl_input_data[offset];
	} else {
		values = jalv_api_ctl_port_data_pointer(ctl_io, request->ctl_index);
	}

	if (zix_shq_read(&jalv->jalv_api.request_queue, values, data_size) != ZIX_STATUS_SUCCESS) {
		ERR("Data segment of change control value request from client ID %u could not be read. Possibly the JALV client sent a corrupted request.", request->client_id);
		return -1;
	}

	DBG("Control %u with size %lu updated by client %u", request->ctl_index, request->data_size, request->client_id);

	return 0;
}


static jalv_api_event_handler_t* jalv_api_ctl_event_handler_of_client(Jalv* const jalv, const unsigned int client_id)
{
	/* search for already existing entry for this client */
	for (unsigned int i=0; i<ARRAY_SIZE(jalv->jalv_api.events); i++) {
		if (jalv->jalv_api.events[i].client_id == client_id) {
			return &jalv->jalv_api.events[i];
		}
	}

	return NULL;
}

static int jalv_api_ctl_subscribe(Jalv* const jalv, const jalv_api_request_t* const request)
{
	char evt_name[request->data_size];
	if (zix_shq_read(&jalv->jalv_api.request_queue, evt_name, sizeof(evt_name)) != ZIX_STATUS_SUCCESS) {
		ERR("Data segment of subscripe request from client %u could not be read. Possibly the JALV client sent a corrupted request.", request->client_id);
		return -1;
	}

	assert(strlen(evt_name) > 0);

	jalv_api_event_handler_t* new_handler = jalv_api_ctl_event_handler_of_client(jalv, request->client_id);
	/* search for first empty entry */
	if (!new_handler) {
		for (unsigned int i=0; i<ARRAY_SIZE(jalv->jalv_api.events); i++) {
			if (jalv->jalv_api.events[i].client_id <= 0) {
				new_handler = &jalv->jalv_api.events[i];
				break;
			}
		}
	}

	if (!new_handler) {
		ERR("No free event queue slot available. Ignoring event %u for %s.\n", request->type, evt_name);
		return 0;
	}

	new_handler->client_id = request->client_id;

	switch (request->type) {
	case JALV_API_SUBSCRIBE_CHANGES:
		if (zix_shq_valid(&new_handler->event_queue)) {
			ERR("JALV API client with ID %u subscribed twice for changes. "
					"Already opened event queue will be replaced with new one.\n", request->client_id);
			zix_shq_destroy(&new_handler->event_queue);
		}
		if (zix_shq_init(&new_handler->event_queue, evt_name, ZIX_MODE_WRONLY | ZIX_MODE_NONBLOCKING, NULL) != ZIX_STATUS_SUCCESS) {
			ERR("Event queue %s cannot be opened for writing. Change subscribe request will be ignored.\n", evt_name);
			return 0;
		}
		break;
	case JALV_API_SIGNAL_CYCLE_DONE:
		if (!zix_sem_valid(&new_handler->cycle_processed_sem)) {
			if (zix_sem_open(&new_handler->cycle_processed_sem, evt_name) != ZIX_STATUS_SUCCESS) {
				ERR("Semaphore %s cannot be opened. Cycle processed will not be signalled.\n", evt_name);
				return 0;
			}
		}
		zix_sem_post(&new_handler->cycle_processed_sem);
		break;
	default:
		ERR("JALV API reqest type %u not supported.\n", request->type);
		return -1;
	}

	return 0;
}


static int jalv_api_ctl_close_client_resources(Jalv* const jalv, const jalv_api_request_t* const request)
{
	assert(request->type == JALV_API_CLIENT_CLOSED);

	jalv_api_event_handler_t* const event_handler = jalv_api_ctl_event_handler_of_client(jalv, request->client_id);
	if (!event_handler) {
		ERR("The client with ID %u has not registered any events. Therefore it is also not required to close any.", request->client_id);
		return 0;
	}

	zix_shq_destroy(&event_handler->event_queue);
	zix_sem_destroy(&event_handler->cycle_processed_sem);
	event_handler->client_id = 0;

	return 1;
}


/**
 * @brief jalv_api_ctl_update
 * @param jalv
 * @return
 * This function will be called from real time context
 */
static int jalv_api_ctl_process_client_requests(Jalv* const jalv)
{
	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);

	/* ensure that api_ctl_lock() was called exactly once before */
	assert(ctl_io);
	assert((ctl_io->guard1 + 1) == ctl_io->guard2);

	/* State changed: JALV_API_CHANGES_BLOCK -> JALV_API_CHANGES_UNBLOCK
	 * A JALV instance has also to unblock
	 * if there is not new request
	 * but the next processing cycle has started
	 *
	 * Only unblock if we are really in the next process cycle.
	 * This guarantees that all JALV instances unblock
	 * in the same period (processing cycle)
	 */
	jalv_shm_t* const jalv_shm = zix_shm_pointer(&jalv->jalv_api_blocking.shm);
	const bool blocking_active = ZIX_ATOMIC_READ(&jalv_shm->blocking_active);
	if (!blocking_active && ctl_io->blocking_active &&
	        jalv_shm->blocking_start_cycle_id !=
	        jalv_backend_get_process_cycle_id(jalv)) {
		void* const shm_ctl_data = jalv_api_ctl_data_pointer(ctl_io);
		memcpy(shm_ctl_data, jalv->jalv_api_blocking.ctl_input_data,
			   jalv->jalv_api_blocking.ctl_input_data_size);
		ctl_io->blocking_active = false;
	}

	while (true) {
		jalv_api_request_t request;
		const ZixStatus ret = zix_shq_read(&jalv->jalv_api.request_queue, &request, sizeof(request));
		if (ret == ZIX_STATUS_EMPTY || ret == ZIX_STATUS_UNAVAILABLE) {
			break;
		} else if (ret != ZIX_STATUS_SUCCESS) {
			return -1;
		}

		/* State changed: JALV_API_CHANGES_UNBLOCK -> JALV_API_CHANGES_BLOCK
		 * Execute this check only
		 * if there is at least one request in the queue.
		 * The switchover is not required for this client
		 * if there are no change requests in the mean time of blocking mode
		 */
		if (blocking_active && !ctl_io->blocking_active) {
			void* const shm_ctl_data = jalv_api_ctl_data_pointer(ctl_io);
			memcpy(jalv->jalv_api_blocking.ctl_input_data, shm_ctl_data,
			       jalv->jalv_api_blocking.ctl_input_data_size);
			ctl_io->blocking_active = true;
		}

		switch (request.type) {
		case JALV_API_CHANGE_VALUE:
			if (jalv_api_ctl_update_controls(jalv, &request) != 0) {
				return -3;
			}
			break;
		case JALV_API_CHANGES_BLOCK:
			if ( ZIX_ATOMIC_EXCHANGE(&jalv_shm->blocking_active, true) ) {
				ERR("JALV API is already locked. (superfluous request from client %u)",
				    request.client_id);
			}
			break;
		case JALV_API_CHANGES_UNBLOCK:
			if (!ZIX_ATOMIC_READ(&jalv_shm->blocking_active)) {
				ERR("JALV API is already unlocked. Request from client %u will be ignored.",
				    request.client_id);
				break;
			}
			/* All JALV instance will unblock in the next
			 * process cycle before processing the audio data.
			 * blocking_start_cycle_id will only be read after
			 * blocking_active == false.
			 * Therefore it is guaranteed that no one is reading
			 * blocking_start_cycle_id when it will be written here.
			 */
			jalv_shm->blocking_start_cycle_id =
			        jalv_backend_get_process_cycle_id(jalv);
			ZIX_ATOMIC_WRITE(&jalv_shm->blocking_active, false);
			break;
		case JALV_API_SUBSCRIBE_CHANGES:
		case JALV_API_SIGNAL_CYCLE_DONE:
			if (jalv_api_ctl_subscribe(jalv, &request) != 0) {
				return -3;
			}
			break;
		case JALV_API_CLIENT_CLOSED:
			jalv_api_ctl_close_client_resources(jalv, &request);
			break;
		default:
			ERR("JALV API reqest type %u not supported.", request.type);
			return -2;
		}
	}

	return 0;
}


int jalv_api_ctl_lock(Jalv* const jalv, const bool process_requests)
{
	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);
	if (!ctl_io) {
		ERR("api_ctl_lock() failed: shm %s not initalized", jalv->jalv_api.shm.name);
		return -1;
	}

	assert(ctl_io->guard1 == ctl_io->guard2);
	ZIX_ATOMIC_ADD(&ctl_io->guard1, 1);
	assert((ctl_io->guard1 + 1) == ctl_io->guard2);


	if (process_requests) {
		/* process internal update requests */
		for (unsigned int i=0; i<jalv->num_ports; i++) {
			struct Port* const port = &jalv->ports[i];
			/* only read new_value for control input ports */
			if (port->type != TYPE_CONTROL || port->flow != FLOW_INPUT) {
				continue;
			}

			const float new_value = ZIX_ATOMIC_EXCHANGE( &(port->control.new_data), NAN );
			if (!isnan(new_value)) {
				*(port->control.data) = new_value;
			}
		}

		/* process external update requests */
		if (jalv_api_ctl_process_client_requests(jalv) != 0) {
			return -2;
		}
	}

	return 0;
}


/**
 * @brief jalv_api_ctl_trigger_events
 * @param jalv
 * @return
 * This function will be called from real time context
 */
static int jalv_api_ctl_trigger_events(Jalv* const jalv)
{
	/* ensure that api_ctl_lock() was called exactly once before */
	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);
	assert(ctl_io);
	assert((ctl_io->guard1 + 1) == ctl_io->guard2);

	const size_t data_size = jalv->ctl_data_size;
	char* const prev_ctl_data = jalv->prev_ctl_data;
	const char* const cur_ctl_data = jalv_api_ctl_data_pointer(ctl_io);

	const unsigned int ctl_count = LV2_CTL_COUNT(ctl_io);
	jalv_api_event_t event[ctl_count];

	// TODO optimization: only check for changes if there is really someone how has subscribed for it
	unsigned int changed_ctl_count = 0;
	for (unsigned int i=0; i<ctl_count; i++) {
		const size_t offset = ctl_io->ctls[i].data_offset;
		const size_t ctl_data_size = ctl_io->ctls[i].data_length;
		if (memcmp(&prev_ctl_data[offset], &cur_ctl_data[offset], ctl_data_size) != 0) {
			event[changed_ctl_count].ctl_index = i;
			changed_ctl_count++;
		}
	}

	/* check if the data has changed */
	if (changed_ctl_count > 0) {
		memcpy(prev_ctl_data, cur_ctl_data, data_size);

		DBG("Control %u changed", event[0].ctl_index);

		/* only send the event elements which are filled with data */
		const size_t events_size = changed_ctl_count * sizeof(event[0]);

		/* signal all subscribed JALV API clients */
		for (unsigned int i=0; i<ARRAY_SIZE(jalv->jalv_api.events); i++) {
			/* check next element if no client is registered in this entry */
			if (jalv->jalv_api.events[i].client_id <= 0) {
				continue;
			}

			DBG("Control %u changed. write to client %u", event[0].ctl_index, jalv->jalv_api.events[i].client_id);

			const ZixStatus err = zix_shq_write(&jalv->jalv_api.events[i].event_queue, event, events_size);
			if (err == ZIX_STATUS_BAD_ARG) {
				/* This slot does not contain a valid queue
				 * continue with the next one
				 */
				continue;
			} else if (err != ZIX_STATUS_SUCCESS) {
				ERR("Write event failed. Removing queue %u from list\n", i);
				zix_shq_destroy(&jalv->jalv_api.events[i].event_queue);
			}
		}
	}

	return 0;
}


/**
 * @brief jalv_api_ctl_unlock
 * @param jalv
 * @return
 * This function will be called from real time context
 */
int jalv_api_ctl_unlock(Jalv* const jalv, const bool trigger_events)
{
	int err = 0;

	if (trigger_events) {
		/* do not fail without unlocking */
		err = jalv_api_ctl_trigger_events(jalv);
	}

	lv2_ctl_io_t* const ctl_io = zix_shm_pointer(&jalv->jalv_api.shm);
	if (!ctl_io) {
		ERR("api_ctl_unlock() failed: shm %s not initalized", jalv->jalv_api.shm.name);
		return -1;
	}

	assert((ctl_io->guard1 + 1) == ctl_io->guard2);
	ZIX_ATOMIC_ADD(&ctl_io->guard2, 1);
	assert(ctl_io->guard1 == ctl_io->guard2);

	return err;
}


void jalv_api_ctl_destroy(Jalv* const jalv)
{
	free(jalv->jalv_api_blocking.ctl_input_data);
	free(jalv->prev_ctl_data);
	zix_shm_destroy(&jalv->jalv_api_blocking.shm);

	jalv_api_ctl_close_internal(&jalv->jalv_api);
}
