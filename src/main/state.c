/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Multi-packet state handling
 * @file main/state.c
 *
 * @ingroup AVP
 *
 * For each round of a multi-round authentication method such as EAP,
 * or a 2FA method such as OTP, a state entry will be created.  The state
 * entry holds data that should be available during the complete lifecycle
 * of the authentication attempt.
 *
 * When a request is complete, #fr_request_to_state is called to transfer
 * ownership of the state VALUE_PAIRs and state_ctx (which the VALUE_PAIRs
 * are allocated in) to a #fr_state_entry_t.  This #fr_state_entry_t holds the
 * value of the State attribute, that will be send out in the response.
 *
 * When the next request is received, #fr_state_to_request is called to transfer
 * the VALUE_PAIRs and state ctx to the new request.
 *
 * The ownership of the state_ctx and state VALUE_PAIRs is transferred as below:
 *
 * @verbatim
   request -> state_entry -> request -> state_entry -> request -> free()
          \-> reply                 \-> reply                 \-> access-reject/access-accept
 * @endverbatim
 *
 * @copyright 2014 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/state.h>
#include <freeradius-devel/rad_assert.h>

/** Holds a state value, and associated VALUE_PAIRs and data
 *
 */
typedef struct state_entry {
	uint64_t		id;				//!< State ID for debugging
	uint8_t			state[AUTH_VECTOR_LEN];		//!< State value in binary.

	time_t			cleanup;			//!< When this entry should be cleaned up.
	struct state_entry	*prev;				//!< Previous entry in the cleanup list.
	struct state_entry	*next;				//!< Next entry in the cleanup list.

	int			tries;

	TALLOC_CTX		*ctx;				//!< ctx to parent any data that needs to be
								//!< tied to the lifetime of the request progression.
	VALUE_PAIR		*vps;				//!< session-state VALUE_PAIRs, parented by ctx.

	request_data_t		*data;				//!< Persistable request data, also parented ctx.
} fr_state_entry_t;

struct fr_state_tree_t {
	uint64_t		id;				//!< Next ID to assign.
	uint32_t		max_sessions;			//!< Maximum number of sessions we track.
	rbtree_t		*tree;				//!< rbtree used to lookup state value.

	fr_state_entry_t	*head, *tail;			//!< Entries to expire.
	uint32_t		timeout;			//!< How long to wait before cleaning up state entires.
#ifdef HAVE_PTHREAD_H
	pthread_mutex_t		mutex;				//!< Synchronisation mutex.
#endif
};

fr_state_tree_t *global_state = NULL;

#ifdef HAVE_PTHREAD_H
#  define PTHREAD_MUTEX_LOCK if (main_config.spawn_workers) pthread_mutex_lock
#  define PTHREAD_MUTEX_UNLOCK if (main_config.spawn_workers) pthread_mutex_unlock
#else
/*
 *	This is easier than ifdef's throughout the code.
 */
#  define PTHREAD_MUTEX_LOCK(_x)
#  define PTHREAD_MUTEX_UNLOCK(_x)
#endif

static void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry);

/** Compare two fr_state_entry_t based on their state value i.e. the value of the attribute
 *
 */
static int state_entry_cmp(void const *one, void const *two)
{
	fr_state_entry_t const *a = one;
	fr_state_entry_t const *b = two;

	return memcmp(a->state, b->state, sizeof(a->state));
}

/** Free the state tree
 *
 */
static int _state_tree_free(fr_state_tree_t *state)
{
	fr_state_entry_t *this;

#ifdef HAVE_PTHREAD_H
	if (main_config.spawn_workers) pthread_mutex_destroy(&state->mutex);
#endif

	DEBUG4("Freeing state tree %p", state);

	while (state->head) {
		this = state->head;
		state_entry_unlink(state, this);
		talloc_free(this);
	}

	for (this = state->head; this; this = this->next) DEBUG4("State %" PRIu64 " needs freeing prev was %" PRIu64 "", this->id, this->prev ? this->prev->id : 100000);

	/*
	 *	Ensure we got *all* the entries
	 */
	rad_assert(!state->head);

	/*
	 *	Free the rbtree
	 */
	rbtree_free(state->tree);

	if (state == global_state) global_state = NULL;

	return 0;
}

/** Initialise a new state tree
 *
 * @param ctx to link the lifecycle of the state tree to.
 * @param max_sessions we track state for.
 * @param timeout How long to wait before cleaning up entries.
 * @return a new state tree or NULL on failure.
 */
fr_state_tree_t *fr_state_tree_init(TALLOC_CTX *ctx, uint32_t max_sessions, uint32_t timeout)
{
	fr_state_tree_t *state;

	state = talloc_zero(NULL, fr_state_tree_t);
	if (!state) return 0;

	state->max_sessions = max_sessions;
	state->timeout = timeout;

	/*
	 *	Create a break in the contexts.
	 *	We still want this to be freed at the same time
	 *	as the parent, but we also need it to be thread
	 *	safe, and multiple threads could be using the
	 *	tree.
	 */
	fr_talloc_link_ctx(ctx, state);

#ifdef HAVE_PTHREAD_H
	if (main_config.spawn_workers && (pthread_mutex_init(&state->mutex, NULL) != 0)) {
		talloc_free(state);
		return NULL;
	}
#endif

	/*
	 *	We need to do controlled freeing of the
	 *	rbtree, so that all the state entries
	 *	are freed before it's destroyed.  Hence
	 *	it being parented from the NULL ctx.
	 */
	state->tree = rbtree_create(NULL, state_entry_cmp, NULL, 0);
	if (!state->tree) {
		talloc_free(state);
		return NULL;
	}
	talloc_set_destructor(state, _state_tree_free);

	return state;
}

/** Unlink an entry and remove if from the tree
 *
 */
static void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry)
{
	fr_state_entry_t *prev, *next;

	prev = entry->prev;
	next = entry->next;

	if (prev) {
		rad_assert(state->head != entry);
		prev->next = next;
	} else if (state->head) {
		rad_assert(state->head == entry);
		state->head = next;
	}

	if (next) {
		rad_assert(state->tail != entry);
		next->prev = prev;
	} else if (state->tail) {
		rad_assert(state->tail == entry);
		state->tail = prev;
	}
	entry->next = NULL;
	entry->prev = NULL;

	rbtree_deletebydata(state->tree, entry);

	DEBUG4("State ID %" PRIu64 " unlinked", entry->id);
}

/** Frees any data associated with a state
 *
 */
static int _state_entry_free(fr_state_entry_t *entry)
{
#ifdef WITH_VERIFY_PTR
	vp_cursor_t cursor;
	VALUE_PAIR *vp;

	/*
	 *	Verify all state attributes are parented
	 *	by the state context.
	 */
	if (entry->ctx) {
		for (vp = fr_cursor_init(&cursor, &entry->vps);
		     vp;
		     vp = fr_cursor_next(&cursor)) {
			rad_assert(entry->ctx == talloc_parent(vp));
		}
	}

	/*
	 *	Ensure any request data is parented by us
	 *	so we know it'll be cleaned up.
	 */
	if (entry->data) rad_assert(request_data_verify_parent(entry, entry->data));
#endif

	/*
	 *	Should also free any state attributes
	 */
	if (entry->ctx) TALLOC_FREE(entry->ctx);

	DEBUG4("State ID %" PRIu64 " freed", entry->id);

	return 0;
}

/** Create a new state entry
 *
 * @note Called with the mutex held.
 */
static fr_state_entry_t *state_entry_create(fr_state_tree_t *state, RADIUS_PACKET *packet, fr_state_entry_t *old)
{
	size_t			i;
	uint32_t		x;
	time_t			now = time(NULL);
	VALUE_PAIR		*vp;
	fr_state_entry_t	*entry, *next;
	fr_state_entry_t	*free_head = NULL, **free_next = &free_head;

	uint8_t			old_state[AUTH_VECTOR_LEN];
	int			old_tries = 0;

	/*
	 *	Clean up old entries.
	 */
	while (state->head && (state->head->cleanup < now)) {
		entry = state->head;

		state_entry_unlink(state, entry);
		*free_next = entry;
		free_next = &(entry->next);
	}

	if (rbtree_num_elements(state->tree) >= (uint32_t) state->max_sessions) return NULL;

	/*
	 *	Record the information from the old state, we may base the
	 *	new state off the old one.
	 *
	 *	Once we release the mutex, the state of old becomes indeterminate
	 *	so we have to grab the values now.
	 */
	if (old) {
		old_tries = old->tries;

		memcpy(old_state, old->state, sizeof(old_state));

		/*
		 *	The old one isn't used any more, so we can free it.
		 */
		if (!old->data) {
			state_entry_unlink(state, old);
			*free_next = old;
		}
	}
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	/*
	 *	Now free the unlinked entries.
	 *
	 *	We do it here as freeing may involve significantly more
	 *	work than just freeing the data.
	 *
	 *	If there's request data that was persisted it will now
	 *	be freed also, and it may have complex destructors associated
	 *	with it.
	 */
	for (next = free_head; next;) {
		entry = next;
		next = entry->next;
		talloc_free(entry);
	}

	/*
	 *	Allocation doesn't need to occur inside the critical region
	 *	and would add significantly to contention.
	 *
	 *	We reparent the talloc chunk later (when we hold the mutex again)
	 *	we can't do it now due to thread safety issues with talloc.
	 */
	entry = talloc_zero(NULL, fr_state_entry_t);
	if (!entry) {
		PTHREAD_MUTEX_LOCK(&state->mutex);	/* Caller expects this to be locked */
		return NULL;
	}
	talloc_set_destructor(entry, _state_entry_free);
	entry->id = state->id++;

	/*
	 *	Limit the lifetime of this entry based on how long the
	 *	server takes to process a request.  Doing it this way
	 *	isn't perfect, but it's reasonable, and it's one less
	 *	thing for an administrator to configure.
	 */
	entry->cleanup = now + state->timeout;

	/*
	 *	Some modules like rlm_otp create their own magic
	 *	state attributes.  If a state value already exists
	 *	int the reply, we use that in preference to the
	 *	old state.
	 */
	vp = fr_pair_find_by_num(packet->vps, PW_STATE, 0, TAG_ANY);
	if (vp) {
		if (DEBUG_ENABLED && (vp->vp_length > sizeof(entry->state))) {
			WARN("State too long, will be truncated.  Expected <= %zd bytes, got %zu bytes",
			     sizeof(entry->state), vp->vp_length);
		}
		memcpy(entry->state, vp->vp_octets, sizeof(entry->state));
	} else {
		/*
		 *	Base the new state on the old state if we had one.
		 */
		if (old) {
			memcpy(entry->state, old_state, sizeof(entry->state));
			entry->tries = old_tries + 1;
		}

		entry->state[0] = entry->tries;
		entry->state[1] = entry->state[0] ^ entry->tries;
		entry->state[8] = entry->state[2] ^ ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 16) & 0xff);
		entry->state[10] = entry->state[2] ^ ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 8) & 0xff);
		entry->state[12] = entry->state[2] ^ (((uint32_t) HEXIFY(RADIUSD_VERSION)) & 0xff);

		/*
		 *	16 octets of randomness should be enough to
		 *	have a globally unique state.
		 */
		for (i = 0; i < sizeof(entry->state) / sizeof(x); i++) {
			x = fr_rand();
			memcpy(entry->state + (i * 4), &x, sizeof(x));
		}

		/*
		 *	Allow a portion ofthe State attribute to be set.
		 *
		 *	This allows load-balancing proxies to be much
		 *	less stateful.
		 */
		if (main_config.state_seed < 256) entry->state[3] = main_config.state_seed;

		vp = fr_pair_afrom_num(packet, PW_STATE, 0);
		fr_pair_value_memcpy(vp, entry->state, sizeof(entry->state));
		fr_pair_add(&packet->vps, vp);
	}

	if (DEBUG_ENABLED4) {
		char hex[(sizeof(entry->state) * 2) + 1];

		fr_bin2hex(hex, entry->state, sizeof(entry->state));

		DEBUG4("State ID %" PRIu64 " created, value 0x%s, expires %" PRIu64 "s",
		       entry->id, hex, (uint64_t)entry->cleanup - now);
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);
	if (rbtree_num_elements(state->tree) >= (uint32_t) state->max_sessions) {
		talloc_free(entry);
		return NULL;
	}

	if (!rbtree_insert(state->tree, entry)) {
		talloc_free(entry);
		return NULL;
	}

	/*
	 *	Link it to the end of the list, which is implicitely
	 *	ordered by cleanup time.
	 */
	if (!state->head) {
		entry->prev = entry->next = NULL;
		state->head = state->tail = entry;
	} else {
		rad_assert(state->tail != NULL);

		entry->prev = state->tail;
		state->tail->next = entry;

		entry->next = NULL;
		state->tail = entry;
	}

	return entry;
}

/** Find the entry, based on the State attribute
 *
 */
static fr_state_entry_t *state_entry_find(fr_state_tree_t *state, RADIUS_PACKET *packet)
{
	VALUE_PAIR *vp;
	fr_state_entry_t *entry, my_entry;

	vp = fr_pair_find_by_num(packet->vps, PW_STATE, 0, TAG_ANY);
	if (!vp) return NULL;

	if (vp->vp_length != sizeof(my_entry.state)) return NULL;

	memcpy(my_entry.state, vp->vp_octets, sizeof(my_entry.state));

	entry = rbtree_finddata(state->tree, &my_entry);

#ifdef WITH_VERIFY_PTR
	if (entry) (void) talloc_get_type_abort(entry, fr_state_entry_t);
#endif

	return entry;
}

/** Called when sending an Access-Accept/Access-Reject to discard state information
 *
 */
void fr_state_discard(fr_state_tree_t *state, REQUEST *request, RADIUS_PACKET *original)
{
	fr_state_entry_t *entry;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find(state, original);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return;
	}
	state_entry_unlink(state, entry);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	TALLOC_FREE(entry);
	request->state = NULL;
	request->state_ctx = NULL;

	return;
}

/** Copy a pointer to the head of the list of state VALUE_PAIRs (and their ctx) into the request
 *
 * @note Does not copy the actual VALUE_PAIRs.  The VALUE_PAIRs and their context
 *	are transferred between state entries as the conversation progresses.
 *
 * @note Called with the mutex free.
 */
void fr_state_to_request(fr_state_tree_t *state, REQUEST *request, RADIUS_PACKET *packet)
{
	fr_state_entry_t *entry;
	TALLOC_CTX *old_ctx = NULL;

	rad_assert(request->state == NULL);

	/*
	 *	No State, don't do anything.
	 */
	if (!fr_pair_find_by_num(request->packet->vps, PW_STATE, 0, TAG_ANY)) {
		RDEBUG3("No &request:State attribute, can't restore &session-state");
		return;
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);

	entry = state_entry_find(state, packet);
	if (entry) {
		if (request->state_ctx) old_ctx = request->state_ctx;

		request->state_ctx = entry->ctx;
		request->state = entry->vps;
		request_data_restore(request, entry->data);

		entry->ctx = NULL;
		entry->vps = NULL;
		entry->data = NULL;
	}

	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	if (request->state) {
		RDEBUG2("Restored &session-state");
		rdebug_pair_list(L_DBG_LVL_2, request, request->state, "&session-state:");
	} else {
		RDEBUG3("No &session-state attributes to restore");
	}

	/*
	 *	Free this outside of the mutex for less contention.
	 */
	if (old_ctx) talloc_free(old_ctx);

	VERIFY_REQUEST(request);
	return;
}


/** Transfer ownership of the state VALUE_PAIRs and ctx, back to a state entry
 *
 * Put request->state into the State attribute.  Put the State attribute
 * into the vps list.  Delete the original entry, if it exists
 *
 * Also creates a new state entry.
 */
bool fr_request_to_state(fr_state_tree_t *state, REQUEST *request, RADIUS_PACKET *original, RADIUS_PACKET *packet)
{
	fr_state_entry_t *entry, *old;
	request_data_t *data;

	request_data_by_persistance(&data, request, true);

	if (request->state && !data) return true;

	if (request->state) {
		RDEBUG2("Saving &session-state");
		rdebug_pair_list(L_DBG_LVL_2, request, request->state, "&session-state:");
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);

	old = original ? state_entry_find(state, original) :
			 NULL;

	entry = state_entry_create(state, packet, old);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return false;
	}

	rad_assert(entry->ctx == NULL);
	rad_assert(request->state_ctx);

	entry->ctx = request->state_ctx;
	entry->vps = request->state;
	entry->data = data;

	request->state_ctx = NULL;
	request->state = NULL;

	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	rad_assert(request->state == NULL);
	VERIFY_REQUEST(request);
	return true;
}
