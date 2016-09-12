/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "fiber.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "say.h"
#include "assoc.h"
#include "memory.h"
#include "trigger.h"
#include "small/pmatomic.h"

static int (*fiber_invoke)(fiber_func f, va_list ap);

#if ENABLE_ASAN
void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                    const void *bottom,
                                    size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save);

#define ASAN_START_SWITCH_FIBER(var_name, will_switch_back, bottom, size) \
	void *var_name = NULL; \
	__sanitizer_start_switch_fiber((will_switch_back) ? &var_name : NULL, \
                                       (bottom), (size))
#define ASAN_FINISH_SWITCH_FIBER(var_name) \
	__sanitizer_finish_switch_fiber(var_name);

#else
#define ASAN_START_SWITCH_FIBER(var_name, will_switch_back, bottom, size)
#define ASAN_FINISH_SWITCH_FIBER(var_name)
#endif

/*
 * Defines a handler to be executed on exit from cord's thread func,
 * accessible via cord()->on_exit (normally NULL). It is used to
 * implement cord_cojoin.
 */
struct cord_on_exit {
	void (*callback)(void*);
	void *argument;
};

/*
 * A special value distinct from any valid pointer to cord_on_exit
 * structure AND NULL. This value is stored in cord()->on_exit by the
 * thread function prior to thread termination.
 */
static const struct cord_on_exit cord_on_exit_sentinel = { NULL, NULL };
#define CORD_ON_EXIT_WONT_RUN (&cord_on_exit_sentinel)

static struct cord main_cord;
__thread struct cord *cord_ptr = NULL;
pthread_t main_thread_id;

static void
update_last_stack_frame(struct fiber *fiber)
{
#ifdef ENABLE_BACKTRACE
	fiber->last_stack_frame = __builtin_frame_address(0);
#else
	(void)fiber;
#endif /* ENABLE_BACKTRACE */

}

static void
fiber_recycle(struct fiber *fiber);

static void
fiber_call_impl(struct fiber *callee)
{
	struct fiber *caller = fiber();
	struct cord *cord = cord();

	/* Ensure we aren't switching to a fiber parked in fiber_loop */
	assert(callee->f != NULL);

	/* Ensure the callee was removed from cord->ready list.
	 * If it wasn't, the callee will observe a 'spurious' wakeup
	 * later, due to a fiber_wakeup() performed in the past.
	 *
	 * To put it another way, fiber_wakeup() is a 'request' to
	 * schedule the fiber for execution, and once it is executing
	 * a wakeup request is considered complete and it must be
	 * removed. */
	assert(rlist_empty(&callee->state));

	assert(caller);
	assert(caller != callee);

	cord->fiber = callee;

	update_last_stack_frame(caller);

	callee->csw++;
	ASAN_START_SWITCH_FIBER(asan_state, 1,
				callee->coro.stack,
				callee->coro.stack_size);
	coro_transfer(&caller->coro.ctx, &callee->coro.ctx);
	ASAN_FINISH_SWITCH_FIBER(asan_state);
}

void
fiber_call(struct fiber *callee)
{
	callee->caller = fiber();
	fiber_call_impl(callee);
}

void
fiber_start(struct fiber *callee, ...)
{
	va_start(callee->f_data, callee);
	fiber_call(callee);
	va_end(callee->f_data);
}

bool
fiber_checkstack()
{
	return false;
}

/**
 * Interrupt a synchronous wait of a fiber inside the event loop.
 * We do so by keeping an "async" event in every fiber, solely
 * for this purpose, and raising this event here.
 *
 * @note: if this is sent to self, followed by a fiber_yield()
 * call, it simply reschedules the fiber after other ready
 * fibers in the same event loop iteration.
 */
void
fiber_wakeup(struct fiber *f)
{
	struct cord *cord = cord();
	if (rlist_empty(&cord->ready)) {
		/*
		 * ev_feed_event(EV_CUSTOM) gets scheduled in the
		 * same event loop iteration, and we rely on this
		 * for quick scheduling. For a wakeup which
		 * actually can invoke a poll() in libev,
		 * use fiber_sleep(0)
		 */
		ev_feed_event(cord->loop, &cord->wakeup_event, EV_CUSTOM);
	}
	/**
	 * Removes the fiber from whatever wait list it is on.
	 *
	 * It's critical that the newly scheduled fiber is
	 * added to the tail of the list, to preserve correct
	 * transaction commit order after a successful WAL write.
	 * (see tx_schedule_commit()/tx_schedule_rollback() in
	 * box/wal.cc)
	 */
	rlist_move_tail_entry(&cord->ready, f, state);
}

/** Cancel the subject fiber.
*
 * Note: cancelation is asynchronous. Use fiber_join() to wait for the
 * cancelation to complete.
 *
 * A fiber may opt to set FIBER_IS_CANCELLABLE to false, and never test
 * that it was cancelled.  Such fiber can not ever be cancelled.
 * However, as long as most of the cooperative code calls
 * fiber_testcancel(), most of the fibers are cancellable.
 *
 * The fiber which is cancelled, has FiberIsCancelled raised
 * in it. For cancellation to work, this exception type should be
 * re-raised whenever (if) it is caught.
 */
void
fiber_cancel(struct fiber *f)
{
	assert(f->fid != 0);
	struct fiber *self = fiber();

	f->flags |= FIBER_IS_CANCELLED;

	/**
	 * Don't wake self and zombies.
	 */
	if (f != self && !fiber_is_dead(f)) {
		if (f->flags & FIBER_IS_CANCELLABLE)
			fiber_wakeup(f);
	}
}

/**
 * Change the current cancellation state of a fiber. This is not
 * a cancellation point.
 */
bool
fiber_set_cancellable(bool yesno)
{
	bool prev = fiber()->flags & FIBER_IS_CANCELLABLE;
	if (yesno == true)
		fiber()->flags |= FIBER_IS_CANCELLABLE;
	else
		fiber()->flags &= ~FIBER_IS_CANCELLABLE;
	return prev;
}

bool
fiber_is_cancelled()
{
	return fiber()->flags & FIBER_IS_CANCELLED;
}

void
fiber_set_joinable(struct fiber *fiber, bool yesno)
{
	if (yesno == true)
		fiber->flags |= FIBER_IS_JOINABLE;
	else
		fiber->flags &= ~FIBER_IS_JOINABLE;
}

/** Report libev time (cheap). */
double
fiber_time(void)
{
	return ev_now(loop());
}

uint64_t
fiber_time64(void)
{
	return (uint64_t) ( ev_now(loop()) * 1000000 + 0.5 );
}

/**
 * Move current fiber to the end of ready fibers list and switch to next
 */
void
fiber_reschedule(void)
{
	fiber_wakeup(fiber());
	fiber_yield();
}

void
fiber_join(struct fiber *fiber)
{
	assert(fiber->flags & FIBER_IS_JOINABLE);

	if (! fiber_is_dead(fiber)) {
		rlist_add_tail_entry(&fiber->wake, fiber(), state);
		fiber_yield();
	}
	assert(fiber_is_dead(fiber));
	bool fiber_was_cancelled = fiber->flags & FIBER_IS_CANCELLED;
	/* Move exception to the caller */
	diag_move(&fiber->diag, &fiber()->diag);
	/** Don't bother with propagation of FiberIsCancelled */
	if (fiber_was_cancelled)
		diag_clear(&fiber()->diag);

	/* The fiber is already dead. */
	fiber_recycle(fiber);
}

/**
 * @note: this is not a cancellation point (@sa fiber_testcancel())
 * but it is considered good practice to call testcancel()
 * after each yield.
 */
void
fiber_yield(void)
{
	struct cord *cord = cord();
	struct fiber *caller = cord->fiber;
	struct fiber *callee = caller->caller;
	caller->caller = &cord->sched;

	/** By convention, these triggers must not throw. */
	if (! rlist_empty(&caller->on_yield))
		trigger_run(&caller->on_yield, NULL);

	cord->fiber = callee;
	update_last_stack_frame(caller);

	callee->csw++;
	ASAN_START_SWITCH_FIBER(asan_state,
				(caller->flags & FIBER_IS_DEAD) == 0,
				callee->coro.stack,
				callee->coro.stack_size);
	coro_transfer(&caller->coro.ctx, &callee->coro.ctx);
	ASAN_FINISH_SWITCH_FIBER(asan_state);
}

struct fiber_watcher_data {
	struct fiber *f;
	bool timed_out;
};

static void
fiber_schedule_timeout(ev_loop *loop,
		       ev_timer *watcher, int revents)
{
	(void) loop;
	(void) revents;

	assert(fiber() == &cord()->sched);
	struct fiber_watcher_data *state =
			(struct fiber_watcher_data *) watcher->data;
	state->timed_out = true;
	fiber_wakeup(state->f);
}

/**
 * @brief yield & check timeout
 * @return true if timeout exceeded
 */
bool
fiber_yield_timeout(ev_tstamp delay)
{
	struct ev_timer timer;
	ev_timer_init(&timer, fiber_schedule_timeout, delay, 0);
	struct fiber_watcher_data state = { fiber(), false };
	timer.data = &state;
	ev_timer_start(loop(), &timer);
	fiber_yield();
	ev_timer_stop(loop(), &timer);
	return state.timed_out;
}

/**
 * Yield the current fiber to events in the event loop.
 */
void
fiber_sleep(double delay)
{
	/*
	 * libev sleeps at least backend_mintime, which is 1 ms in
	 * case of poll()/Linux, unless there are idle watchers.
	 * So, to properly implement fiber_sleep(0), i.e. a sleep
	 * with a zero timeout, we set up an idle watcher, and
	 * it triggers libev to poll() with zero timeout.
	 */
	if (delay == 0) {
		ev_idle_start(loop(), &cord()->idle_event);
	}
	/*
	 * We don't use fiber_wakeup() here to ensure there is
	 * no infinite wakeup loop in case of fiber_sleep(0).
	 */
	fiber_yield_timeout(delay);

	if (delay == 0) {
		ev_idle_stop(loop(), &cord()->idle_event);
	}
}

void
fiber_schedule_cb(ev_loop *loop, ev_watcher *watcher, int revents)
{
	(void) loop;
	(void) revents;
	struct fiber *fiber = watcher->data;
	assert(fiber() == &cord()->sched);
	fiber_wakeup(fiber);
}

static inline void
fiber_schedule_list(struct rlist *list)
{
	struct fiber *first;
	struct fiber *last;

	assert(! rlist_empty(list));

	first = last = rlist_shift_entry(list, struct fiber, state);

	while (! rlist_empty(list)) {
		last->caller = rlist_shift_entry(list, struct fiber, state);
		last = last->caller;
	}
	last->caller = fiber();
	fiber_call_impl(first);
}

static void
fiber_schedule_wakeup(ev_loop *loop, ev_async *watcher, int revents)
{
	(void) loop;
	(void) watcher;
	(void) revents;
	struct cord *cord = cord();
	fiber_schedule_list(&cord->ready);
}

static void
fiber_schedule_idle(ev_loop *loop, ev_idle *watcher,
		    int revents)
{
	(void) loop;
	(void) watcher;
	(void) revents;
}


struct fiber *
fiber_find(uint32_t fid)
{
	struct mh_i32ptr_t *fiber_registry = cord()->fiber_registry;
	mh_int_t k = mh_i32ptr_find(fiber_registry, fid, NULL);

	if (k == mh_end(fiber_registry))
		return NULL;
	return (struct fiber *) mh_i32ptr_node(fiber_registry, k)->val;
}

static void
register_fid(struct fiber *fiber)
{
	struct mh_i32ptr_node_t node = { fiber->fid, fiber };
	mh_i32ptr_put(cord()->fiber_registry, &node, NULL, NULL);
}

static void
unregister_fid(struct fiber *fiber)
{
	struct mh_i32ptr_node_t node = { fiber->fid, NULL };
	mh_i32ptr_remove(cord()->fiber_registry, &node, NULL);
}

void
fiber_gc(void)
{
	if (region_used(&fiber()->gc) < 128 * 1024) {
		region_reset(&fiber()->gc);
		return;
	}

	region_free(&fiber()->gc);
}

/** Common part of fiber_new() and fiber_recycle(). */
static void
fiber_reset(struct fiber *fiber)
{
	rlist_create(&fiber->on_yield);
	rlist_create(&fiber->on_stop);
	fiber->flags = FIBER_DEFAULT_FLAGS;
}

/** Destroy an active fiber and prepare it for reuse. */
static void
fiber_recycle(struct fiber *fiber)
{
	/* no exceptions are leaking */
	assert(diag_is_empty(&fiber->diag));
	/* no pending wakeup */
	assert(rlist_empty(&fiber->state));
	fiber_reset(fiber);
	fiber->gc.name[0] = '\0';
	fiber->f = NULL;
	memset(fiber->fls, 0, sizeof(fiber->fls));
	unregister_fid(fiber);
	fiber->fid = 0;
	region_free(&fiber->gc);
	rlist_move_entry(&cord()->dead, fiber, link);
}

static void
fiber_loop(void *data __attribute__((unused)))
{
	ASAN_FINISH_SWITCH_FIBER(NULL);
	for (;;) {
		struct fiber *fiber = fiber();

		assert(fiber != NULL && fiber->f != NULL && fiber->fid != 0);
		if (fiber_invoke(fiber->f, fiber->f_data) != 0) {
			struct error *e = diag_last_error(&fiber->diag);
			/* diag must not be empty on error */
			assert(e != NULL || fiber->flags & FIBER_IS_CANCELLED);
			/*
			 * For joinable fibers, it's the business
			 * of the caller to deal with the error.
			 */
			if (!(fiber->flags & FIBER_IS_JOINABLE)) {
				if (!(fiber->flags & FIBER_IS_CANCELLED))
					error_log(e);
				diag_clear(&fiber()->diag);
			}
		} else {
			/*
			 * Make sure a leftover exception does not
			 * propagate up to the joiner.
			 */
			diag_clear(&fiber()->diag);
		}
		fiber->flags |= FIBER_IS_DEAD;
		while (! rlist_empty(&fiber->wake)) {
		       struct fiber *f;
		       f = rlist_shift_entry(&fiber->wake, struct fiber,
					     state);
		       fiber_wakeup(f);
	        }
		if (! rlist_empty(&fiber->on_stop))
			trigger_run(&fiber->on_stop, fiber);
		/* no pending wakeups */
		assert(rlist_empty(&fiber->state));
		if (! (fiber->flags & FIBER_IS_JOINABLE))
			fiber_recycle(fiber);
		/*
		 * Crash if spurious wakeup happens, don't call the old
		 * function again, ap is garbage by now.
		 */
		fiber->f = NULL;
		fiber_yield();	/* give control back to scheduler */
	}
}

/** Set fiber name.
 *
 * @param[in] name the new name of the fiber. Truncated to
 * FIBER_NAME_MAXLEN.
*/

void
fiber_set_name(struct fiber *fiber, const char *name)
{
	assert(name != NULL);
	region_set_name(&fiber->gc, name);
}

extern inline void
fiber_set_key(struct fiber *fiber, enum fiber_key key, void *value);

extern inline void *
fiber_get_key(struct fiber *fiber, enum fiber_key key);

/**
 * Create a new fiber.
 *
 * Takes a fiber from fiber cache, if it's not empty.
 * Can fail only if there is not enough memory for
 * the fiber structure or fiber stack.
 *
 * The created fiber automatically returns itself
 * to the fiber cache when its "main" function
 * completes.
 */
struct fiber *
fiber_new(const char *name, fiber_func f)
{
	struct cord *cord = cord();
	struct fiber *fiber = NULL;

	if (! rlist_empty(&cord->dead)) {
		fiber = rlist_first_entry(&cord->dead,
					  struct fiber, link);
		rlist_move_entry(&cord->alive, fiber, link);
	} else {
		fiber = (struct fiber *)
			mempool_alloc(&cord->fiber_mempool);
		if (fiber == NULL) {
			diag_set(OutOfMemory, sizeof(struct fiber),
				 "fiber pool", "fiber");
			return NULL;
		}
		memset(fiber, 0, sizeof(struct fiber));

		if (tarantool_coro_create(&fiber->coro, &cord->slabc,
					  fiber_loop, NULL)) {
			mempool_free(&cord->fiber_mempool, fiber);
			return NULL;
		}

		region_create(&fiber->gc, &cord->slabc);

		rlist_create(&fiber->state);
		rlist_create(&fiber->wake);
		diag_create(&fiber->diag);
		fiber_reset(fiber);

		rlist_add_entry(&cord->alive, fiber, link);
	}

	fiber->f = f;
	/* fids from 0 to 100 are reserved */
	if (++cord->max_fid < 100)
		cord->max_fid = 101;
	fiber->fid = cord->max_fid;
	fiber_set_name(fiber, name);
	register_fid(fiber);

	return fiber;
}

/**
 * Free as much memory as possible taken by the fiber.
 *
 * Sic: cord()->sched needs manual destruction in
 * cord_destroy().
 */
void
fiber_destroy(struct cord *cord, struct fiber *f)
{
	if (f == fiber()) {
		/** End of the application. */
		assert(cord == &main_cord);
		return;
	}
	assert(f != &cord->sched);

	trigger_destroy(&f->on_yield);
	trigger_destroy(&f->on_stop);
	rlist_del(&f->state);
	region_destroy(&f->gc);
	tarantool_coro_destroy(&f->coro, &cord->slabc);
	diag_destroy(&f->diag);
}

void
fiber_destroy_all(struct cord *cord)
{
	struct fiber *f;
	rlist_foreach_entry(f, &cord->alive, link)
		fiber_destroy(cord, f);
	rlist_foreach_entry(f, &cord->dead, link)
		fiber_destroy(cord, f);
}

/* {{{ fiber_pool */

static void
fiber_pool_fetch_output(struct fiber_pool *pool)
{
	tt_pthread_mutex_lock(&pool->mutex);
	stailq_concat(&pool->output, &pool->pipe);
	tt_pthread_mutex_unlock(&pool->mutex);
}


static void
fiber_pool_idle_cb(ev_loop *loop, struct ev_timer *watcher, int events)
{
	(void) events;
	struct fiber_pool *pool = (struct fiber_pool *) watcher->data;
	if (! rlist_empty(&pool->idle)) {
		struct fiber *f;
		/*
		 * Schedule the fiber at the tail of the list,
		 * it's the one most likely to have not been
		 * scheduled lately.
		 */
		f = rlist_shift_tail_entry(&pool->idle, struct fiber, state);
		fiber_call(f);
	}
	ev_timer_again(loop, watcher);
}

/** Create fibers to handle all outstanding tasks. */
static void
fiber_pool_cb(ev_loop *loop, struct ev_async *watcher, int events)
{
	(void) loop;
	(void) events;
	struct fiber_pool *pool = (struct fiber_pool *) watcher->data;
	fiber_pool_fetch_output(pool);

	struct stailq *output = &pool->output;
	while (! stailq_empty(output)) {
		struct fiber *f;
		if (! rlist_empty(&pool->idle)) {
			f = rlist_shift_entry(&pool->idle, struct fiber, state);
			fiber_call(f);
		} else if (pool->size < pool->max_size) {
			f = fiber_new(cord_name(cord()), pool->f);
			if (f == NULL) {
				error_log(diag_last_error(&fiber()->diag));
				break;
			}
			fiber_start(f, pool);
		} else {
			/**
			 * No worries that this watcher may not
			 * get scheduled again - there are enough
			 * worker fibers already, so just leave.
			 */
			break;
		}
	}
}
void
fiber_pool_destroy(struct fiber_pool *pool)
{
	/*
	 * Do not destroy async or idle timers, or fibers:
	 * events are destroyed along with the event loop,
	 * and fibers are freed at once when thread runtime
	 * pool is destroyed.
         */
	(void) tt_pthread_mutex_destroy(&pool->mutex);
}

void
fiber_pool_create(struct fiber_pool *pool, int max_pool_size,
		  float idle_timeout, fiber_func f)
{
	pool->consumer = loop();
	pool->f = f;
	pool->idle_timeout = idle_timeout;
	rlist_create(&pool->idle);
	ev_timer_init(&pool->idle_timer, fiber_pool_idle_cb, 0,
		      pool->idle_timeout);
	pool->idle_timer.data = pool;
	ev_timer_again(loop(), &pool->idle_timer);
	pool->size = 0;
	pool->max_size = max_pool_size;
	stailq_create(&pool->output);
	stailq_create(&pool->pipe);
	ev_async_init(&pool->fetch_output, fiber_pool_cb);
	pool->fetch_output.data = pool;
	ev_async_start(pool->consumer, &pool->fetch_output);

	(void) tt_pthread_mutex_init(&pool->mutex, NULL);
}

/* }}} */

void
cord_create(struct cord *cord, const char *name)
{
	cord() = cord;
	memset(&cord->fiber_pool, 0, sizeof(cord->fiber_pool));
	slab_cache_set_thread(&cord()->slabc);

	cord->id = pthread_self();
	cord->on_exit = NULL;
	slab_cache_create(&cord->slabc, &runtime);
	mempool_create(&cord->fiber_mempool, &cord->slabc,
		       sizeof(struct fiber));
	rlist_create(&cord->alive);
	rlist_create(&cord->ready);
	rlist_create(&cord->dead);
	cord->fiber_registry = mh_i32ptr_new();

	/* sched fiber is not present in alive/ready/dead list. */
	cord->sched.fid = 1;
	fiber_reset(&cord->sched);
	diag_create(&cord->sched.diag);
	region_create(&cord->sched.gc, &cord->slabc);
	fiber_set_name(&cord->sched, "sched");
	cord->fiber = &cord->sched;

	cord->max_fid = 100;
	/*
	 * No need to start this event since it's only used for
	 * ev_feed_event(). Saves a few cycles on every
	 * event loop iteration.
	 */
	ev_async_init(&cord->wakeup_event, fiber_schedule_wakeup);

	ev_idle_init(&cord->idle_event, fiber_schedule_idle);
	cord_set_name(name);

	/* Record stack extents */
	cord->sched.coro.stack_size = pthread_get_stacksize_np(cord->id);
	cord->sched.coro.stack = pthread_get_stackaddr_np(cord->id);
}

void
cord_destroy(struct cord *cord)
{
	slab_cache_set_thread(&cord->slabc);
	if (cord->loop)
		ev_loop_destroy(cord->loop);
	/* Only clean up if initialized. */
	if (cord->fiber_registry) {
		fiber_destroy_all(cord);
		mh_i32ptr_delete(cord->fiber_registry);
	}
	region_destroy(&cord->sched.gc);
	diag_destroy(&cord->sched.diag);
	if (cord->fiber_pool.f)
		fiber_pool_destroy(&cord->fiber_pool);
	slab_cache_destroy(&cord->slabc);
}

struct cord_thread_arg
{
	struct cord *cord;
	const char *name;
	void *(*f)(void *);
	void *arg;
	bool is_started;
	pthread_mutex_t start_mutex;
	pthread_cond_t start_cond;
};

/**
 * Cord main thread function. It's not exception-safe, the
 * body function must catch all exceptions instead.
 */
void *cord_thread_func(void *p)
{
	struct cord_thread_arg *ct_arg = (struct cord_thread_arg *) p;
	cord_create(ct_arg->cord, (ct_arg->name));
	/** Can't possibly be the main thread */
	assert(cord()->id != main_thread_id);
	tt_pthread_mutex_lock(&ct_arg->start_mutex);
	void *(*f)(void *) = ct_arg->f;
	void *arg = ct_arg->arg;
	ct_arg->is_started = true;
	tt_pthread_cond_signal(&ct_arg->start_cond);
	tt_pthread_mutex_unlock(&ct_arg->start_mutex);
	void *res = f(arg);
	/*
	 * cord()->on_exit initially holds NULL. This field is
	 * change-once.
	 * Either handler installation succeeds (in cord_cojoin())
	 * or prior to thread exit the thread function discovers
	 * that no handler was installed so far and it stores
	 * CORD_ON_EXIT_WONT_RUN to prevent a future handler
	 * installation (since a handler won't run anyway).
	 */
	const struct cord_on_exit *handler = NULL; /* expected value */
	bool changed;

	changed = pm_atomic_compare_exchange_strong(&cord()->on_exit,
	                                            &handler,
	                                            CORD_ON_EXIT_WONT_RUN);
	if (!changed)
		handler->callback(handler->argument);
	return res;
}

int
cord_start(struct cord *cord, const char *name, void *(*f)(void *), void *arg)
{
	int res = -1;
	struct cord_thread_arg ct_arg = { cord, name, f, arg, false,
		PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };
	tt_pthread_mutex_lock(&ct_arg.start_mutex);
	if (!(cord->loop = ev_loop_new(EVFLAG_AUTO | EVFLAG_ALLOCFD)))
		goto end;
	if (tt_pthread_create(&cord->id, NULL, cord_thread_func, &ct_arg))
		goto end;
	res = 0;
	while (! ct_arg.is_started)
		tt_pthread_cond_wait(&ct_arg.start_cond, &ct_arg.start_mutex);
end:
	if (res != 0) {
		if (cord->loop) {
			ev_loop_destroy(cord->loop);
			cord->loop = NULL;
		}
	}
	tt_pthread_mutex_unlock(&ct_arg.start_mutex);
	tt_pthread_mutex_destroy(&ct_arg.start_mutex);
	tt_pthread_cond_destroy(&ct_arg.start_cond);
	return res;
}

int
cord_join(struct cord *cord)
{
	assert(cord() != cord); /* Can't join self. */
	void *retval = NULL;
	int res = tt_pthread_join(cord->id, &retval);
	if (res == 0) {
		/*
		 * cord_thread_func guarantees that
		 * cord->exception is only set if the subject cord
		 * has terminated with an uncaught exception,
		 * transfer it to the caller. If there is
		 * no exception, this clears the caller's
		 * diagnostics area.
		 */
		diag_move(&cord->fiber->diag, &fiber()->diag);
	}
	cord_destroy(cord);
	return res;
}

/** The state of the waiter for a thread to complete. */
struct cord_cojoin_ctx
{
	struct ev_loop *loop;
	/** Waiting fiber. */
	struct fiber *fiber;
	/*
	 * This event is signalled when the subject thread is
	 * about to die.
	 */
	struct ev_async async;
	bool task_complete;
};

static void
cord_cojoin_on_exit(void *arg)
{
	struct cord_cojoin_ctx *ctx = (struct cord_cojoin_ctx *)arg;

	ev_async_send(ctx->loop, &ctx->async);
}

static void
cord_cojoin_wakeup(struct ev_loop *loop, struct ev_async *ev, int revents)
{
	(void)loop;
	(void)revents;

	struct cord_cojoin_ctx *ctx = (struct cord_cojoin_ctx *)ev->data;

	ctx->task_complete = true;
	fiber_wakeup(ctx->fiber);
}

int
cord_cojoin(struct cord *cord)
{
	assert(cord() != cord); /* Can't join self. */

	struct cord_cojoin_ctx ctx;
	ctx.loop = loop();
	ctx.fiber = fiber();
	ctx.task_complete = false;

	ev_async_init(&ctx.async, cord_cojoin_wakeup);
	ctx.async.data = &ctx;
	ev_async_start(loop(), &ctx.async);

	struct cord_on_exit handler = { cord_cojoin_on_exit, &ctx };

	/*
	 * cord->on_exit initially holds a NULL value. This field is
	 * change-once.
	 */
	const struct cord_on_exit *prev_handler = NULL; /* expected value */
	bool changed = pm_atomic_compare_exchange_strong(&cord->on_exit,
	                                                 &prev_handler,
	                                                 &handler);
	/*
	 * A handler installation fails either if the thread did exit or
	 * if someone is already joining this cord (BUG).
	 */
	if (!changed) {
		/* Assume cord's thread already exited. */
		assert(prev_handler == CORD_ON_EXIT_WONT_RUN);
	} else {
		/*
		 * Wait until the thread exits. Prior to exit the
		 * thread invokes cord_cojoin_on_exit, signaling
		 * ev_async, making the event loop call
		 * cord_cojoin_wakeup, waking up this fiber again.
		 *
		 * The fiber is non-cancellable during the wait to
		 * avoid invalidating of the cord_cojoin_ctx
		 * object declared on stack.
		 */
		bool cancellable = fiber_set_cancellable(false);
		fiber_yield();
		/* Spurious wakeup indicates a severe BUG, fail early. */
		if (ctx.task_complete == 0)
			panic("Wrong fiber woken");
		fiber_set_cancellable(cancellable);
	}

	ev_async_stop(loop(), &ctx.async);
	return cord_join(cord);
}

void
break_ev_loop_f(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	ev_break(loop(), EVBREAK_ALL);
}

struct costart_ctx
{
	fiber_func run;
	void *arg;
};

/** Replication acceptor fiber handler. */
static void *
cord_costart_thread_func(void *arg)
{
	struct costart_ctx ctx = *(struct costart_ctx *) arg;
	free(arg);

	struct fiber *f = fiber_new("main", ctx.run);
	if (f == NULL)
		return NULL;

	struct trigger break_ev_loop = {
		RLIST_LINK_INITIALIZER, break_ev_loop_f, NULL, NULL
	};
	/*
	 * Got to be in a trigger, to break the loop even
	 * in case of an exception.
	 */
	trigger_add(&f->on_stop, &break_ev_loop);
	fiber_set_joinable(f, true);
	fiber_start(f, ctx.arg);
	if (!fiber_is_dead(f)) {
		/* The fiber hasn't died right away at start. */
		ev_run(loop(), 0);
	}
	/*
	 * Preserve the exception with which the main fiber
	 * terminated, if any.
	 */
	assert(fiber_is_dead(f));
	fiber_join(f);

	return NULL;
}

int
cord_costart(struct cord *cord, const char *name, fiber_func f, void *arg)
{
	/** Must be allocated to avoid races. */
	struct costart_ctx *ctx = (struct costart_ctx *) malloc(sizeof(*ctx));
	if (ctx == NULL)
		return -1;
	ctx->run = f;
	ctx->arg = arg;
	if (cord_start(cord, name, cord_costart_thread_func, ctx) == -1) {
		free(ctx);
		return -1;
	}
	return 0;
}

void
cord_set_name(const char *name)
{
	snprintf(cord()->name, sizeof cord()->name, "%s", name);
	/* Main thread's name will replace process title in ps, skip it */
	if (cord_is_main())
		return;
	tt_pthread_setname(name);
}

bool
cord_is_main()
{
	return cord() == &main_cord;
}

struct slab_cache *
cord_slab_cache(void)
{
	return &cord()->slabc;
}

void
fiber_init(int (*invoke)(fiber_func f, va_list ap))
{
	fiber_invoke = invoke;
	main_thread_id = pthread_self();
	main_cord.loop = ev_default_loop(EVFLAG_AUTO | EVFLAG_ALLOCFD);
	cord_create(&main_cord, "main");
}

void
fiber_free(void)
{
	cord_destroy(&main_cord);
}

int fiber_stat(fiber_stat_cb cb, void *cb_ctx)
{
	struct fiber *fiber;
	struct cord *cord = cord();
	int res;
	rlist_foreach_entry(fiber, &cord->alive, link) {
		res = cb(fiber, cb_ctx);
		if (res != 0)
			return res;
	}
	return 0;
}
