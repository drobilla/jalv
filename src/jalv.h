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

#ifndef JALV_API
#define JALV_API

/**
   @addtogroup API
   @{
   @name Control
   @{
*/

#include <assert.h>
#include "zix/atomic.h"
#include "zix/shm.h"
#include "zix/shq.h"
#include "zix/sem.h"


/**
 * @brief JALV_API_MAX_EVENT_SUBSCRIBER is the maximal number of JALV API clients
 * how can subscribe for changes on the same JALV instance
 * at the same time
 */
#define JALV_API_MAX_EVENT_SUBSCRIBER 10

/* A unique string has to be added to this prefix
 * If using with JACK the JACK client name has to be appended.
 * If using with audio port the JALV process id has to be appended
 */
#define JALV_API_PREFIX     "jalv-"
#define JALV_API_SHM_PREFIX JALV_API_PREFIX "shm-"
#define JALV_API_REQ_PREFIX JALV_API_PREFIX "req-"
#define JALV_API_EVT_PREFIX JALV_API_PREFIX "evt-"
#define JALV_API_SEM_PREFIX JALV_API_PREFIX "sem-"

/**
 * @brief JALV_SHM_READ_TIMEOUT_US is the timeout
 * for reading the shared memory.
 * Default: 1sec
 */
#define JALV_SHM_READ_TIMEOUT_US            1e6
#define JALV_READ_ITERATIONS                1000


typedef enum {
	JALV_API_PORT_TYPE_DEFAULT,
	JALV_API_PORT_TYPE_TOGGLE,
	JALV_API_PORT_TYPE_INTEGER,
	JALV_API_PORT_TYPE_ENUMERATION,
	JALV_API_PORT_TYPE_BYTES,
} lv2_ctl_data_type_t;

typedef struct {
	char name[NAME_MAX];
	lv2_ctl_data_type_t type;
	float min;
	float max;
	/**
	 * @brief data_offset is the position of the data of this control port
	 * This is the index for lv2_ctl_io_t::data[]
	 */
	size_t data_offset;
	size_t data_length;

	/**
	 * @brief scale_points_offset is the position of first scale point of this control port
	 * This is the index for lv2_ctl_io_t::data[]
	 * This value is only valid
	 * if lv2_ctl_io_data_t::type == JALV_API_PORT_TYPE_ENUMERATION
	 *
	 */
	size_t scale_points_offset;
	size_t scale_points_count;
} lv2_ctl_io_data_t;


typedef struct {
	float value;
	char label[NAME_MAX];
} lv2_ctl_scale_point_t;


/**
 * @brief This structure defines the content of the shared memory.
 * The size of the shared memory will be dynamically detected
 * depending on the size and amount of the control ports.
 * The following sections are part of the dynamic region:
 * lv2_ctl_io_data_t[lv2_ctl_io_t::in_ctl_count]
 * lv2_ctl_io_data_t[lv2_ctl_io_t::out_ctl_count]
 * char control_data[sum(lv2_ctl_io_data_t::data_length)]
 * lv2_ctl_scale_point_t[sum(lv2_ctl_io_data_t::scale_points_count)]
 */
typedef struct {
	/**
	 * @brief guard1 and guard2 will be used to protect the shared memory.
	 * guard1 will be incremented before the audio real time process writes to the shared memory
	 */
	atomic_uint guard1;
	/**
	 * @brief guard2 will be incremented after the audio real time process has written to the shared memory
	 * Therefore when ever guard1 and guard2 is not equal reading of the shared memory is not allowed and
	 * could result in invalid data.
	 */
	atomic_uint guard2;
	/**
	 * @brief blocking_active if true all new requests will be ignored and
	 * not written to this shared memory.
	 * The changes will apply on the next porcessing cycle
	 * when a JALV_API_CHANGES_UNBLOCK request was sent by any JALV instance.
	 * This variable is atomic to allow reading it with unequal guards.
	 */
	bool blocking_active;
	unsigned int in_ctl_count;
	unsigned int out_ctl_count;
	union {
		lv2_ctl_io_data_t ctls[0];
		/**
		 * @brief data contains all data structures with variable amount
		 * Do not access this variable directly.
		 * Use jalv_api_ctl_port_data_pointer() to acces the control port data instead
		 */
		char data[0];
		/* lv2_ctl_scale_point_t scale_points[0];
		 * To access the scale points use jalv_api_ctl_port_scale_point()
		 */
	};
} lv2_ctl_io_t;

#define LV2_CTL_COUNT(ctl_io) ((ctl_io)->in_ctl_count + (ctl_io)->out_ctl_count)


typedef enum {
	JALV_API_PORT_ID_INDEX,
	JALV_API_PORT_ID_SYMBOL,
} jalv_api_port_id_type_t;

typedef struct {
	jalv_api_port_id_type_t type;
	union {
		unsigned int ctl_index;
		const char* symbol;
	};
} jalv_api_port_id_t;


typedef enum {
	JALV_API_CHANGE_VALUE,
	/**
	 * @brief JALV_API_CHANGES_BLOCK holds all JALV_API_CHANGE_VALUE events
	 * They will be processed if JALV_API_CHANGES_UNBLOCK was sent.
	 */
	JALV_API_CHANGES_BLOCK,
	/**
	 * @brief JALV_API_CHANGES_UNBLOCK unblocks the JALV_API_CHANGE_VALUE events.
	 * These events will be applied with the next processing cycle in sync with
	 * all other JALV instances of the same JACK instance.
	 */
	JALV_API_CHANGES_UNBLOCK,
	JALV_API_SUBSCRIBE_CHANGES,
	JALV_API_SIGNAL_CYCLE_DONE,
	JALV_API_CLIENT_CLOSED,
} jalv_api_request_type_t;


/**
 * @brief jalv_api_request_t describes the structure of an request
 * The length of an request is depending on the appended data.
 */
typedef struct {
	jalv_api_request_type_t type;
	/**
	 * @brief client_id identifies the client which created this request uniquely
	 */
	unsigned int client_id;
	unsigned int ctl_index;
	size_t data_size;
	/**
	 * @brief data
	 * JALV_API_CHANGE_VALUE: the new values for the control
	 * JALV_API_SUBSCRIBE_CHANGES: the name of the event queue
	 * JALV_API_SIGNAL_CYCLE_DONE: the name of the semaphore
	 */
	char data[0];
} jalv_api_request_t;


typedef struct {
	unsigned int ctl_index;
} jalv_api_event_t;


typedef struct {
	unsigned int client_id;
	ZixShq event_queue;
	ZixSem cycle_processed_sem;
} jalv_api_event_handler_t;

typedef struct {
	char user_group[NAME_MAX];
	/* only used by JALV API client */
	char evt_name[NAME_MAX];

	ZixShm shm;
	ZixShq request_queue;
	/* The JALV API clients are using only the first entry
	 * The other entries are only used by JALV internally
	 */
	jalv_api_event_handler_t events[JALV_API_MAX_EVENT_SUBSCRIBER];
} jalv_api_t;


/**
 * @brief jalv_api_ctl_open will open all IPC mechanism required
 * to communicate with the JALV instance
 * @param jalv_api
 * @param name
 * @param group
 * @return <0 on error
 */
static inline int jalv_api_ctl_open(jalv_api_t* const jalv_api, const char* const name, const char* const group)
{
	/* at least jalv_api->user_group
	 * has to be zero if it is not used
	 */
	memset(jalv_api, 0, sizeof(*jalv_api));

	if (group) {
		strncpy(jalv_api->user_group, group, sizeof(jalv_api->user_group));
		jalv_api->user_group[sizeof(jalv_api->user_group) - 1] = 0;
	}


	STRCAT(shm_name, JALV_API_SHM_PREFIX, name);

	if (zix_shm_init(&jalv_api->shm, shm_name, ZIX_MODE_RDONLY, 0, NULL, false) != ZIX_STATUS_SUCCESS) {
		ERR("zix_shm_init(%s) failed\n", shm_name);
		return -1;
	}


	STRCAT(req_name, JALV_API_REQ_PREFIX, name);

	/* read access is required for polling */
	if (zix_shq_init(&jalv_api->request_queue, req_name, ZIX_MODE_RDWR, NULL) != ZIX_STATUS_SUCCESS) {
		ERR("zix_shq_init(%s) failed\n", req_name);
		return -1;
	}

	const int pid = zix_pid();
	const int size = snprintf(jalv_api->evt_name, sizeof(jalv_api->evt_name),
	         JALV_API_EVT_PREFIX "%s-%u", name, pid);
	if ((size_t)size >= sizeof(jalv_api->evt_name)) {
		ERR("The instance name '%s' is too long. Therefore the event name '%s' is possibly not unique.\n",
		    name, jalv_api->evt_name);
		return -2;
	}


	/* create the event queue only if the JALV API client is requesting it
	 * It can be requested by calling jalv_api_ctl_event_handle()
	 */
	for (unsigned int i=0; i<ARRAY_SIZE(jalv_api->events); i++) {
		zix_shq_clear(&jalv_api->events[i].event_queue);
		zix_sem_clear(&jalv_api->events[i].cycle_processed_sem);
	}

	return 0;
}


static inline int jalv_api_ctl_request_internal(const jalv_api_t* const jalv_api, const jalv_api_request_type_t type, const char* const name)
{
	const size_t name_size = name ? (strlen(name) + 1) : 0;
	char buffer[sizeof(jalv_api_request_t) + name_size];

	jalv_api_request_t* const request = (jalv_api_request_t*)buffer;
	request->type = type;
	request->client_id = zix_pid();
	request->data_size = name_size;
	if (name) {
		memcpy(request->data, name, name_size);
	}

	const ZixStatus ret = zix_shq_write(&jalv_api->request_queue, buffer, sizeof(buffer));
	if (ret != ZIX_STATUS_SUCCESS) {
		ERR("Sending request(%u) failed with %s", type, zix_strerror(ret));
		return -1;
	}

	return 0;
}


/**
 * @brief jalv_api_ctl_wait_for_pending_requests waits for all requests
 * which were sent before calling this function.
 * @param jalv_api
 * @return <0 on error
 */
static inline int jalv_api_ctl_wait_for_pending_requests(jalv_api_t* const jalv_api)
{
	/* If there are no requests currently in the queue
	 * there is no need to block till the next cycle was processed
	 */
	if (zix_shq_wait_for_data(&jalv_api->request_queue, 0) != ZIX_STATUS_EMPTY) {
		const int pid = zix_pid();
		char sem_name[NAME_MAX-4];
		snprintf(sem_name, sizeof(sem_name), JALV_API_SEM_PREFIX "%u", pid);

		/* only create the semaphore if not already done */
		if (!zix_sem_valid(&jalv_api->events[0].cycle_processed_sem)) {
			const ZixStatus ret = zix_sem_create(&jalv_api->events[0].cycle_processed_sem, sem_name, 0, jalv_api->user_group);
			if (ret != ZIX_STATUS_SUCCESS) {
				ERR("zix_sem_create() failed with %s",
				    zix_strerror(ret));
				return -1;
			}
		}

		if (jalv_api_ctl_request_internal(jalv_api, JALV_API_SIGNAL_CYCLE_DONE, sem_name) < 0) {
			return -1;
		}

		/* wait for the requested signal */
		if (zix_sem_wait(&jalv_api->events[0].cycle_processed_sem) != ZIX_STATUS_SUCCESS) {
			return -2;
		}
	}

	return 0;
}


/**
 * @brief api_ctl_read
 * @param jalv_api
 * @param enforce_read if true ctl_data will always be overwritten.
 * Use false only if ctl_data was read at least once.
 * The data structure contains a version counter which has to be initialized valid.
 * If false the ctl_data will only be updated if the data has changed.
 * @param ctl_data needs always to have the size of the shared memory
 * This size can be read with zix_shm_size()
 * @return
 */
static inline int jalv_api_ctl_read(const jalv_api_t* const jalv_api, const bool enforce_read, lv2_ctl_io_t* const ctl_data)
{
	if (!jalv_api || !ctl_data) {
		return -1;
	}

	lv2_ctl_io_t* const shm_ctl_data = zix_shm_pointer(&jalv_api->shm);
	if (!shm_ctl_data) {
		ERR("Shared memory not yet available for reading (fd %d)", jalv_api->shm.fd);
		return -2;
	}

	/* Do not read the shared memory
	 * if the data has not yet changed.
	 * The data can only be changed by the audio real time thread
	 * and this thread will increment both guards when writting
	 * to the shared memory.
	 */
	const unsigned int start_version = ZIX_ATOMIC_READ(&shm_ctl_data->guard1);
	if (!enforce_read && start_version == ctl_data->guard1) {
		return 0;
	}

	const size_t size = zix_shm_size(&jalv_api->shm);
	for (int i=0; i<JALV_READ_ITERATIONS; i++) {
		for (int k=0; k<1000; k++) {
			const unsigned int guard2 = ZIX_ATOMIC_READ(&shm_ctl_data->guard2);
			memcpy(ctl_data, shm_ctl_data, size);
			const unsigned int guard1 = ZIX_ATOMIC_READ(&shm_ctl_data->guard1);

			if (guard1 == guard2) {
				/* copying was successfully */
				return 1;
			}
		}

		zix_sleep_us(JALV_SHM_READ_TIMEOUT_US / JALV_READ_ITERATIONS);
	}

	const unsigned int end_version = ZIX_ATOMIC_READ(&shm_ctl_data->guard1);
	const unsigned int guard2 = ZIX_ATOMIC_READ(&shm_ctl_data->guard2);
	ERR("Read timeout. (start shm version %u, end shm version %u, guard2 %u)", start_version, end_version, guard2);
	return -4;
}


/**
 * @brief api_ctl_data_pointer
 * @param ctl_io the shared memory region
 * @param ctl_index the index in the shared memory region. This can differ to the Port::index.
 * @return the pointer to the start of the control value buffer
 */
static inline void* jalv_api_ctl_port_data_pointer(lv2_ctl_io_t* const ctl_io, const unsigned int ctl_index)
{
	if (!ctl_io) {
		return NULL;
	}

	const lv2_ctl_io_data_t* const data = &ctl_io->ctls[ctl_index];
	const size_t data_start = LV2_CTL_COUNT(ctl_io) * sizeof(ctl_io->ctls[0]);

	return &ctl_io->data[data_start + data->data_offset];
}


/**
 * @brief jalv_api_ctl_port_scale_point
 * @param ctl_io
 * @param ctl_index is the indec of the control
 * @param sp_index is the index for the scale point of this control
 * @return the scale point referenced by sp_index
 */
static inline lv2_ctl_scale_point_t* jalv_api_ctl_port_scale_point(lv2_ctl_io_t* const ctl_io, const unsigned int ctl_index, const unsigned int sp_index)
{
	if (!ctl_io) {
		return NULL;
	}

	const lv2_ctl_io_data_t* const data = &ctl_io->ctls[ctl_index];
	assert(data->type == JALV_API_PORT_TYPE_ENUMERATION);
	/* at least two scale points has to be defined */
	assert(data->scale_points_count >= 2);

	if (sp_index >= data->scale_points_count) {
		/* end of iteration reached */
		return NULL;
	}

	lv2_ctl_scale_point_t* const scale_points = (lv2_ctl_scale_point_t*)&ctl_io->data[data->scale_points_offset];
	return &scale_points[sp_index];
}


/**
 * @brief jalv_api_ctl_port_scale_point_next iterates over the list of scale points of a control port
 * @param ctl_io is the pointer to the shared memory
 * @param ctl_index is the control port index number
 * @param iterator a counter which will incremented by this function.
 * Has to be initalized with 0 and should not be touched in the mean while of iteration
 * @return
 */
static inline const lv2_ctl_scale_point_t* jalv_api_ctl_port_scale_point_next(const lv2_ctl_io_t* const ctl_io, const unsigned int ctl_index, unsigned int* const iterator)
{
	const lv2_ctl_scale_point_t* const scale_point = jalv_api_ctl_port_scale_point(ctl_io, ctl_index, *iterator);
	(*iterator)++;

	return scale_point;
}


static inline int jalv_api_symbol2ctl_index(const lv2_ctl_io_t* const ctl_data, const char* const name)
{
	for (unsigned int i=0; i<LV2_CTL_COUNT(ctl_data); i++) {
		if (strncmp(ctl_data->ctls[i].name, name, sizeof(ctl_data->ctls[i].name)) == 0) {
			return i;
		}
	}

	ERR("Symbol name '%s' not found!\n", name);
	return -1;
}


static inline int jalv_api_ctl_read_port(const jalv_api_t* const jalv_api, const jalv_api_port_id_t* const id, const size_t data_size, void* const data) {
	if (!jalv_api || !data || data_size <= 0) {
		return -1;
	}

	char buffer[zix_shm_size(&jalv_api->shm)];
	lv2_ctl_io_t* const temp_ctl_data = (lv2_ctl_io_t*)buffer;
	/* In this case at least three different sections
	 * (lv2_ctl_io_t, lv2_ctl_io_data_t[ctl_index] and the data)
	 * has to be read.
	 * Therefore the read access would be much more complex.
	 * Additionally in case of a symbol2ctl_index conversion
	 * all lv2_ctl_io_data_t elements are required.
	 */
	if (jalv_api_ctl_read(jalv_api, true, temp_ctl_data) < 1) {
		return -2;
	}

	int ctl_index = -1;
	switch (id->type) {
	case JALV_API_PORT_ID_INDEX:
		ctl_index = id->ctl_index;
		break;
	case JALV_API_PORT_ID_SYMBOL:
		ctl_index = jalv_api_symbol2ctl_index(temp_ctl_data, id->symbol);
		break;
	default:
		return -3;
	}

	if (ctl_index < 0) {
		return -4;
	}

	if (data_size > temp_ctl_data->ctls[ctl_index].data_length) {
		return -5;
	}

	void* const ctl_data = jalv_api_ctl_port_data_pointer(temp_ctl_data, ctl_index);
	memcpy(data, ctl_data, data_size);
}


/**
 * @brief jalv_api_ctl_read_port reads only the data of a control port
 * from the shared memory
 * @param jalv_api
 * @param ctl_index
 * @param data_size
 * @param data
 * @return
 */
static inline int jalv_api_ctl_read_port_by_index(const jalv_api_t* const jalv_api, const unsigned int ctl_index, const size_t data_size, void* const data)
{
	jalv_api_port_id_t id;
	id.type = JALV_API_PORT_ID_INDEX;
	id.ctl_index = ctl_index;

	return jalv_api_ctl_read_port(jalv_api, &id, data_size, data);
}

/**
 * @brief jalv_api_ctl_read_port_by_symbol reads only the data of an control port
 * from the shared memory
 * @param jalv_api
 * @param symbol is the symbol name of the control port where the data should be read
 * @param data_size
 * @param data
 * @return
 */
static inline int jalv_api_ctl_read_port_by_symbol(const jalv_api_t* const jalv_api, const char* const symbol, const size_t data_size, void* const data)
{
	jalv_api_port_id_t id;
	id.type = JALV_API_PORT_ID_SYMBOL;
	id.symbol = symbol;

	return jalv_api_ctl_read_port(jalv_api, &id, data_size, data);
}


/**
 * @brief jalv_api_ctl_write creates a request to change the control data in the shared memory
 * @param jalv_api
 * @param ctl_index
 * @param ctl_values
 * @param size
 * @return
 */
static inline int jalv_api_ctl_write(const jalv_api_t* const jalv_api, const unsigned int ctl_index, void* const ctl_values, const size_t size)
{
	if (!jalv_api || !ctl_values) {
		return -1;
	}

	char buffer[sizeof(jalv_api_request_t) + size];

	jalv_api_request_t* const request = (jalv_api_request_t*)buffer;
	request->type = JALV_API_CHANGE_VALUE;
	request->client_id = zix_pid();
	request->ctl_index = ctl_index;
	request->data_size = size;
	memcpy(request->data, ctl_values, size);

	if (zix_shq_write(&jalv_api->request_queue, buffer, sizeof(buffer)) != ZIX_STATUS_SUCCESS) {
		return -1;
	}

	return 0;
}


/**
 * @brief jalv_api_ctl_value_count
 * @param ctl_io
 * @param ctl_index
 * @return the number of values available on this control port.
 * This is only valid for control ports which are using the default data type float
 */
static inline unsigned int jalv_api_ctl_value_count(const lv2_ctl_io_t* const ctl_io, const unsigned int ctl_index)
{
	if (!ctl_io) {
		return 0;
	}

	const lv2_ctl_io_data_t* const data = &ctl_io->ctls[ctl_index];

	return (data->data_length / sizeof(float));
}


/**
 * @brief jalv_api_ctl_event_handle
 * @param jalv_api
 * @return the event handle for changed control values
 */
static inline int jalv_api_ctl_event_handle(jalv_api_t* const jalv_api)
{
	if (!zix_shq_valid(&jalv_api->events[0].event_queue)) {
		if (zix_shq_init(&jalv_api->events[0].event_queue, jalv_api->evt_name, ZIX_MODE_CREATE | ZIX_MODE_RDONLY | ZIX_MODE_NONBLOCKING, jalv_api->user_group) != ZIX_STATUS_SUCCESS) {
			ERR("zix_shq_init(%s) failed (group %s)\n", jalv_api->evt_name, jalv_api->user_group);
			return -1;
		}

		/* register event queue */
		if (jalv_api_ctl_request_internal(jalv_api, JALV_API_SUBSCRIBE_CHANGES, jalv_api->evt_name) < 0) {
			return -2;
		}

		/* wait till the event queue was registered and
		 * events can be sent to this client
		 */
		if (jalv_api_ctl_wait_for_pending_requests(jalv_api) < 0) {
			ERR("Waiting for registration of event queue failed. Possibly some events will not received by this JALV API client %u.", zix_pid());
		}
	}

	return zix_shq_handle(&jalv_api->events[0].event_queue);
}


/**
 * @brief jalv_api_ctl_event_read reads one event from the event queue
 * @param jalv_api
 * @param event
 * @return
 * jalv_api_ctl_event_handle() has to be called before
 */
static inline int jalv_api_ctl_event_read(jalv_api_t* const jalv_api, jalv_api_event_t* const event)
{
	const ZixStatus ret = zix_shq_read(&jalv_api->events[0].event_queue, event, sizeof(*event));
	if (ret == ZIX_STATUS_EMPTY) {
		return 0;
	} else if (ret != ZIX_STATUS_SUCCESS) {
		if (ret == ZIX_STATUS_UNAVAILABLE) {
			ERR("The event queue %d was closed by the opposite side.", zix_shq_handle(&jalv_api->events[0].event_queue));
		}
		return -1;
	}

	return 1;
}


/**
 * @brief jalv_api_ctl_write_sequence_begin starts a write sequence.
 * The following write requests of all JALV instances will be blocked until
 * @ref jalv_api_ctl_write_sequence_commit() will be called.
 * The changes will be applied before processing the next audio period.
 * @param jalv_api
 * @return
 */
static inline int jalv_api_ctl_write_sequence_begin(const jalv_api_t* const jalv_api)
{
	return jalv_api_ctl_request_internal(jalv_api, JALV_API_CHANGES_BLOCK, NULL);
}

/**
 * @brief jalv_api_ctl_write_sequence_commit will apply a write sequence.
 * See @ref jalv_api_ctl_write_sequence_begin() for more details.
 * @param jalv_api
 * @return
 */
static inline int jalv_api_ctl_write_sequence_commit(const jalv_api_t* const jalv_api)
{
	return jalv_api_ctl_request_internal(jalv_api, JALV_API_CHANGES_UNBLOCK, NULL);
}

/**
 * @brief jalv_api_ctl_write_sequence_active
 * @param jalv_api
 * @return true if a write sequence was started with
 * @ref jalv_api_ctl_write_sequence_begin() by any JALV instance.
 * @return false if no write sequence is active. In this case writes will be
 * directly applied before the next audio processing cycle of this JALV instance.
 * There is no synchronisation between the JALV instances active.
 */
static inline bool jalv_api_ctl_write_sequence_active(jalv_api_t* const jalv_api)
{
	char buffer[zix_shm_size(&jalv_api->shm)];
	lv2_ctl_io_t* const ctl_data = (lv2_ctl_io_t*)buffer;
	// TODO read only sizeof(lv2_ctl_io_t)
	if (jalv_api_ctl_read(jalv_api, true, ctl_data) < 0) {
		return false;
	}

	return ctl_data->blocking_active;
}


static inline void jalv_api_ctl_close_internal(jalv_api_t* const jalv_api)
{
	if (!jalv_api)
		return;

	for (unsigned int i=0; i<ARRAY_SIZE(jalv_api->events); i++) {
		zix_shq_destroy(&jalv_api->events[i].event_queue);
		zix_sem_destroy(&jalv_api->events[i].cycle_processed_sem);
	}
	zix_shq_destroy(&jalv_api->request_queue);
	zix_shm_destroy(&jalv_api->shm);
}


/**
 * @brief api_ctl_close cleans up all resources of the API
 * @param jalv
 */
static inline void jalv_api_ctl_close(jalv_api_t* const jalv_api)
{
	if (!jalv_api) {
		ERR("Closing JALV API of NULL object (%p). Seems to be an implementation issue.", (void*)jalv_api);
		return;
	}

	/* only trigger the close request if there is any resource to close
	 * This reduces the processing overhead of the real time thread of JALV
	 */
	const bool is_event_queue_valid = zix_shq_valid(&jalv_api->events[0].event_queue);
	if (is_event_queue_valid || zix_sem_valid(&jalv_api->events[0].cycle_processed_sem)) {
		jalv_api_request_t request;
		memset(&request, 0, sizeof(request));
		request.type = JALV_API_CLIENT_CLOSED;
		request.client_id = zix_pid();

		if (zix_shq_write(&jalv_api->request_queue, &request, sizeof(request)) != ZIX_STATUS_SUCCESS) {
			ERR("Could not request to close all ressources of client %u in the JALV instance!", request.client_id);
		}

		/* wait till JALV closes the event queue if existing */
		if (is_event_queue_valid) {
			if (zix_shq_wait_for_closed(&jalv_api->events[0].event_queue, 1000) != ZIX_STATUS_SUCCESS) {
				ERR("JALV has not closed the event queue of this client with ID %u in time.", request.client_id);
			}
		}
	}

	jalv_api_ctl_close_internal(jalv_api);
}

/**
 * @page jalv_api Control API
 *
 * To ensure that the real time audio processing thread is
 * not blocked by any JALV API client
 * a lock-free inter process communication is used.
 * The data exchange from the JALV instance
 * to the JALV API client is done via a shared memory.
 * The data exchange from the JALV API client to the JALV instance
 * is done via a queue.
 * <br>
 * The Lv2 plug-in will directly access the shared memory
 * but due to the fact that the implementation of the Lv2 plug-in is not in our hand
 * it cannot be guaranteed that the shared memory will be accessed in an atomic way
 * by the Lv2 plug-ins.
 * Therefore the JALV API client cannot write directly into the shared memory and
 * has to use the request queue to change control port values.
 * Then JALV is synchronizing the access of the Lv2 plug-in and
 * the request queue to the shared memory.
 * The request queue is also requered to guarantee atomic writes of multiple bytes.
 * <br>
 * The different communication mechanism which are involved
 * are described in the following diagram.
 * @startuml Inter process communication
 *   left to right direction
 *   skinparam componentStyle uml2
 *   skinparam packageStyle rect
 *
 *   (Shared memory) as SHM
 *   (Request queue) as REQ
 *
 *   (Event queue 1) as EVT1
 *   (Request done semaphore 1) as SEM1
 *
 *   [JALV] as JALV
 *   SHM <-- JALV : read, write
 *   REQ --> JALV : receive
 *   EVT1 <-- JALV : send
 *   SEM1 <-- JALV : signal
 *
 *   [JALV API client 1] as CLIENT1
 *   CLIENT1 <-- SHM : read-only
 *   CLIENT1 --> REQ : send
 *   CLIENT1 <-- EVT1 : receive
 *   CLIENT1 <-- SEM1 : wait
 *
 *   [JALV API client 2] as CLIENT2
 *   CLIENT2 <-- SHM : read-only
 *   CLIENT2 --> REQ : send
 * @enduml
 *
 * The interactions with this communication mechanisms are shown in the next diagram.
 * @startuml Reading and writing control ports
 * actor "JALV API client" as CLIENT
 * database "Shared memory" as SHM
 * participant "Request queue" as REQ
 * participant JALV
 * participant "Audio backend thread" as BE
 * participant "Lv2 plug-in" as LV2
 *
 * activate JALV
 * create LV2
 * JALV -> LV2: init()
 * create BE
 * JALV -> BE: init()
 *
 *
 * CLIENT -> SHM: jalv_api_ctl_read()
 * activate SHM
 * loop guard1 != guard2
 * SHM -> SHM: read()
 * end
 * deactivate SHM
 *
 *
 * CLIENT -> REQ: jalv_api_ctl_write()
 *
 *
 * activate BE
 * BE -> JALV: process()
 * activate JALV
 *
 * JALV -> SHM: guard1++ [lock SHM]
 * activate SHM
 * loop process all requests
 * JALV -> REQ: zix_shq_read()
 * JALV -> SHM: write()
 * end
 * JALV -> LV2: run()
 * JALV -> SHM: guard2++ [unlock SHM]
 * deactivate SHM
 *
 * deactivate JALV
 * deactivate BE
 * @enduml
 *
 * The shared memory is never locked in a blocking why for JALV.
 * JALV is always allowed to write to the shared memory.
 * To guarantee that the clients do not read invalid data
 * two atomic version counters (@ref lv2_ctl_io_t guard1 and @ref lv2_ctl_io_t guard2) will be used.
 * The first version counter will be incremented before JALV changes the content of the shared memory (by @ref jalv_api_ctl_lock()).
 * The second version counter will be incremented after JALV has changed the content of the shared memory (by @ref jalv_api_ctl_unlock()).
 * Therefore it is only allowed to read the data of the shared memory as long as @ref lv2_ctl_io_t guard1 equals @ref lv2_ctl_io_t guard2.
 * Only the JALV audio real time thread is allowed to write data to the shared memory.
 * The audio real time thread is also allowed to read the data when @ref lv2_ctl_io_t guard1 does not equal @ref lv2_ctl_io_t guard2
 * because this thread is the only one which writes to the shared memory it will never read invalid data.
 * <br>
 * Due to the limitations of the shared memory access the JALV API clients are not allowed to write to the shared memory.
 * There are different requests which can be sent to JALV via the request queue.
 * One of these requests is a write request.
 * Such a write request is defined by @ref jalv_api_request_t.
 * The different types of the requests are defined by @ref jalv_api_request_type_t.
 * Due to the fact a write will not directly written to the shared memory a pending read could read the old data.
 * To omit this a JALV API client should wait till the write took effect before reading the same value again.
 * This can be achieved with the help of the signalling semaphore.
 * The same shared memory and request queue will be accessed by all JALV API clients.
 *
 * @page event Wait for control port changes
 * If an JALV API client wants to wait for the change of a control port value
 * it can create an event queue and register this queue on JALV.
 * JALV will write an event to this queue for each changed control port.
 * The event handle which can be used for polling will be returned by
 * @ref jalv_api_ctl_event_handle().
 * The pending events can be read with @ref jalv_api_ctl_event_read().
 *
 * @page wait Block until all writes were applied
 * If an JALV API client wants to wait till a request took effect
 * if can wait for an semaphore.
 * This semaphore has to be created by the client and
 * can be registered on JALV.
 * To simplify this procedure the @ref jalv_api_ctl_wait_for_pending_requests() function can be used.
 *
 * @page Sequence Atomic write of multiple control ports
 * The following digram shows the correct sequence for
 * appling multiple control port changes before the same audio period.
 * @startuml Atomic write of multiple control ports
 * actor "JALV API client 1" as CLIENT
 * actor "JALV API client 2" as CLIENT2
 * participant "Request queue" as REQ
 * participant JALV
 * participant "Audio backend thread" as BE
 * participant "Lv2 plug-in" as LV2
 *
 * activate JALV
 * create LV2
 * JALV -> LV2: init()
 * create BE
 * JALV -> BE: init()
 *
 * activate CLIENT
 * CLIENT -> REQ: jalv_api_ctl_write_sequence_begin()
 *
 * CLIENT -> REQ: jalv_api_ctl_write()
 *
 * activate BE
 * BE -> JALV: process()
 * activate JALV
 * alt not jalv_api_ctl_write_sequence_active()
 * loop process all requests
 * JALV -> REQ: zix_shq_read()
 * end
 * end
 * JALV -> LV2: run()
 *
 * deactivate JALV
 * deactivate BE
 *
 * CLIENT2 -> REQ: jalv_api_ctl_write()
 * CLIENT -> REQ: jalv_api_ctl_write()
 *
 * CLIENT -> REQ: jalv_api_ctl_write_sequence_commit()
 * deactivate CLIENT
 *
 *
 * activate BE
 * BE -> JALV: process()
 * activate JALV
 * alt not jalv_api_ctl_write_sequence_active()
 * loop process all requests
 * JALV -> REQ: zix_shq_read()
 * end
 * end
 * JALV -> LV2: run()
 * @enduml
 *
 * Pending write requests will only be processed
 * when the write sequence is not active.
 * This can be checked with @ref jalv_api_ctl_write_sequence_active().
 * All control port changes between @ref jalv_api_ctl_write_sequence_begin() and
 * @ref jalv_api_ctl_write_sequence_commit() will be applied
 * before the same audio processing period.
 * This includes all control port changes of all JALV instances
 * of the same JACK instance.
 * The status which is returned by jalv_api_ctl_write_sequence_active()
 * is synchronized between all JALV instances.
 */


/**
   @endcond
   @}
   @}
*/

#endif // JALV_API
