/*
 * Copyright 2009, Michael Lotz, mmlr@mlotz.ch.
 * Copyright 2026 KunanyiOS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "config.h"
#include "WKDeclarationSpecifiers.h"

#include <locks.h>

#include <OS.h>

#include <cstddef>

#include <user_thread.h>

extern "C" status_t _kern_block_thread(uint32 flags, bigtime_t timeout);


// Haiku's libroot rw_lock wait loop can lose progress after SIGCHLD or
// another signal interrupts _kern_block_thread. Export just the two acquisition
// functions so libbe's existing looper locks use the corrected wait path in
// WebKit processes. Keep them in libWebKit, a direct dependency of the browser
// and helpers: placing them only in its JavaScriptCore dependency lets libroot
// win native symbol lookup first. Initialization, release and recursion remain
// native, and no lock state is duplicated between libraries.
// These private headers must match the target Haiku development environment.
typedef struct rw_lock_waiter {
	rw_lock_waiter *	next;
	thread_id			thread;
	bool				writer;
} rw_lock_waiter;


static status_t
rw_lock_wait(rw_lock* lock, bool writer)
{
    rw_lock_waiter waiter;
    waiter.thread = find_thread(nullptr);
    waiter.next = nullptr;
    waiter.writer = writer;
    if (lock->waiters)
        lock->last_waiter->next = &waiter;
    else
        lock->waiters = &waiter;
    lock->last_waiter = &waiter;

    for (;;) {
        // Restore the wait state while holding the queue's mutex. A signal
        // interruption leaves B_INTERRUPTED in it; retrying the syscall alone
        // cannot block again and makes a later native wakeup ineffective.
        get_user_thread()->wait_status = 1;
        mutex_unlock(&lock->lock);
        status_t result = _kern_block_thread(0, 0);
        mutex_lock(&lock->lock);
        if (result != B_INTERRUPTED)
            return result;
        // Unlock may have granted this waiter while the syscall was returning
        // from interruption. Its removal is the protected grant record; do not
        // sleep again after that handoff or increment reader counts twice.
        bool pending = false;
        for (auto* current = lock->waiters; current; current = current->next) {
            if (current == &waiter) {
                pending = true;
                break;
            }
        }
        if (!pending)
            return B_OK;
    }
}

WK_EXPORT status_t
__rw_lock_read_lock(rw_lock *lock)
{
	MutexLocker locker(lock->lock);

	if (lock->writer_count == 0) {
		lock->reader_count++;
		return B_OK;
	}

	if (lock->holder == find_thread(NULL)) {
		lock->owner_count++;
		return B_OK;
	}

	return rw_lock_wait(lock, false);
}


WK_EXPORT status_t
__rw_lock_write_lock(rw_lock *lock)
{
	MutexLocker locker(lock->lock);

	if (lock->reader_count == 0 && lock->writer_count == 0) {
		lock->writer_count++;
		lock->holder = find_thread(NULL);
		lock->owner_count = 1;
		return B_OK;
	}

	if (lock->holder == find_thread(NULL)) {
		lock->owner_count++;
		return B_OK;
	}

	lock->writer_count++;

	status_t result = rw_lock_wait(lock, true);
	if (result != B_OK)
		return result;

	if (lock->holder != find_thread(NULL)) {
		debugger("write locked but holder not set");
		return B_ERROR;
	}

	lock->owner_count = 1;
	return B_OK;
}

