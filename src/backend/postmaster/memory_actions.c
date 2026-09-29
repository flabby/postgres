/*-------------------------------------------------------------------------
 * memory_actions.c -- bounded user-session registry and cooperative actions
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "postmaster/memory_protector.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/timestamp.h"

#define MP_ACTION_LIFETIME_MS 1000

typedef struct MPActionSlot
{
	MPCandidate candidate;
	MPIdentity	target;
	MPAction	action;
	uint64		deadline;
	uint64		serial;
	bool		statement_bound;
	bool		requires_pressure;
	ProcNumber	worker_cursor;
} MPActionSlot;
typedef struct MPActionsShared
{
	slock_t		lock;
	uint64		next_generation;
	uint64		next_action;
	uint64		cancel_requests;
	uint64		terminate_requests;
	MPActionSlot slots[FLEXIBLE_ARRAY_MEMBER];
} MPActionsShared;
static MPActionsShared *actions;
static uint64 my_generation;
static volatile sig_atomic_t action_pending;
MPActionSafeHook memory_protection_action_safe_hook = NULL;

Size
MPActionsShmemSize(void)
{
	return add_size(offsetof(MPActionsShared, slots),
					mul_size(MaxBackends, sizeof(MPActionSlot)));
}

void
MPActionsShmemInit(void)
{
	bool		found;

	actions = ShmemInitStruct("Memory protection sessions", MPActionsShmemSize(), &found);
	if (!found)
	{
		memset(actions, 0, MPActionsShmemSize());
		SpinLockInit(&actions->lock);
	}
}

static bool
same_session(const MPIdentity *a, const MPIdentity *b)
{
	return a->pid == b->pid && a->proc_number == b->proc_number &&
		a->generation == b->generation && a->epoch == b->epoch;
}

static void
unregister_backend(int code, Datum arg)
{
	MPActionSlot *slot = &actions->slots[MyProcNumber];

	SpinLockAcquire(&actions->lock);
	if (slot->candidate.id.generation == my_generation)
		memset(slot, 0, sizeof(*slot));
	SpinLockRelease(&actions->lock);
	my_generation = 0;
	action_pending = false;
}

static void
register_backend(bool privileged, const MPIdentity *leader)
{
	MPActionSlot *slot;
	uint64		ticks = 0;

	if (!IsUnderPostmaster || (!AmRegularBackendProcess() && leader == NULL) ||
		MyProcNumber < 0 || MyProcNumber >= MaxBackends)
		return;
	MPReadProcessStart(MyProcPid, &ticks);
	slot = &actions->slots[MyProcNumber];
	SpinLockAcquire(&actions->lock);
	memset(slot, 0, sizeof(*slot));
	slot->candidate.id.epoch = (uint64) PgStartTime;
	slot->candidate.id.generation = ++actions->next_generation;
	slot->candidate.id.proc_number = MyProcNumber;
	slot->candidate.id.pid = MyProcPid;
	slot->candidate.start_ticks = ticks;
	slot->candidate.eligible = !privileged && leader == NULL;
	if (leader && leader->pid != 0 && leader->proc_number >= 0 &&
		leader->proc_number < MaxBackends)
	{
		MPCandidate *parent = &actions->slots[leader->proc_number].candidate;

		if (same_session(leader, &parent->id) && parent->active &&
			leader->statement == parent->id.statement)
			slot->candidate.leader = *leader;
	}
	my_generation = slot->candidate.id.generation;
	SpinLockRelease(&actions->lock);
	before_shmem_exit(unregister_backend, (Datum) 0);
}

void
MPBackendRegister(bool privileged)
{
	register_backend(privileged, NULL);
}

void
MPParallelRegister(const MPIdentity *leader)
{
	register_backend(true, leader);
}

void
MPStatementBegin(void)
{
	MPActionSlot *slot;

	if (!my_generation)
		return;
	slot = &actions->slots[MyProcNumber];
	SpinLockAcquire(&actions->lock);
	slot->candidate.id.statement++;
	slot->candidate.active = true;
	/* A cancellation can never follow a connection into its next statement. */
	if (slot->statement_bound)
	{
		slot->action = MP_ACTION_NONE;
		slot->serial = ++actions->next_action;
		slot->candidate.pending = false;
	}
	SpinLockRelease(&actions->lock);
}

void
MPStatementEnd(void)
{
	MPActionSlot *slot;

	if (!my_generation)
		return;
	slot = &actions->slots[MyProcNumber];
	SpinLockAcquire(&actions->lock);
	slot->candidate.active = false;
	if (slot->statement_bound)
	{
		slot->action = MP_ACTION_NONE;
		slot->serial = ++actions->next_action;
		slot->candidate.pending = false;
	}
	SpinLockRelease(&actions->lock);
}

void
MPBackendIdle(bool in_transaction)
{
	if (!my_generation)
		return;
	MPStatementEnd();
	SpinLockAcquire(&actions->lock);
	actions->slots[MyProcNumber].candidate.in_transaction = in_transaction;
	SpinLockRelease(&actions->lock);
}

bool
MPReadCandidate(ProcNumber slot, MPCandidate *out)
{
	uint64		now = MPMonotonicMillis();

	if (slot < 0 || slot >= MaxBackends)
		return false;
	SpinLockAcquire(&actions->lock);
	*out = actions->slots[slot].candidate;
	out->pending = actions->slots[slot].action != MP_ACTION_NONE && now <= actions->slots[slot].deadline;
	SpinLockRelease(&actions->lock);
	return out->id.pid != 0;
}

static bool
pressure_is_current(const MPObservation *o, uint64 now)
{
	return o->mode == MP_MODE_ENFORCE && o->sample.valid &&
		o->sample.boundary_trusted && o->sample.sampled_at_ms <= now &&
		now - o->sample.sampled_at_ms <= MP_SAMPLE_MAX_AGE_MS &&
		(o->state == MP_PRESSURE || o->state == MP_CRITICAL);
}

static bool
queue_action(const MPIdentity *id, MPAction action, bool statement_bound, bool requires_pressure)
{
	MPActionSlot *slot;
	uint64		now = MPMonotonicMillis();
	bool		accept = false;

	if (memory_protection_mode != MP_MODE_ENFORCE ||
		(action != MP_ACTION_CANCEL && action != MP_ACTION_TERMINATE) ||
		id->proc_number < 0 || id->proc_number >= MaxBackends)
		return false;
	slot = &actions->slots[id->proc_number];
	SpinLockAcquire(&actions->lock);
	if (same_session(id, &slot->candidate.id) && slot->candidate.eligible &&
		(slot->action == MP_ACTION_NONE || now > slot->deadline ||
		 (action == MP_ACTION_TERMINATE && slot->action == MP_ACTION_CANCEL)) &&
		(!statement_bound ||
		 (slot->candidate.active && id->statement == slot->candidate.id.statement)))
	{
		slot->serial = ++actions->next_action;
		slot->statement_bound = statement_bound;
		slot->requires_pressure = requires_pressure;
		slot->target = *id;
		slot->action = action;
		slot->deadline = now + MP_ACTION_LIFETIME_MS;
		slot->candidate.pending = true;
		if (action == MP_ACTION_CANCEL)
			actions->cancel_requests++;
		else
			actions->terminate_requests++;
		accept = true;
	}
	SpinLockRelease(&actions->lock);
	if (accept)
	{
		if (SendProcSignal(id->pid, PROCSIG_MEMORY_PROTECTION, id->proc_number) != 0)
			return false;		/* Retained request expires; never retarget a
								 * PID. */
	}
	return accept;
}

bool
MPQueueAction(const MPIdentity *id, MPAction action)
{
	return queue_action(id, action, action == MP_ACTION_CANCEL, false);
}

bool
MPQueueEscalation(const MPIdentity *id)
{
	return queue_action(id, MP_ACTION_TERMINATE, true, false);
}

void
MPHandleInterrupt(void)
{
	action_pending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

void
MPProcessInterrupt(void)
{
	MPActionSlot *slot;
	MPAction	action;
	uint64		serial;
	uint64		now;
	MPObservation observation;

	if (!action_pending)
		return;
	if (!my_generation)
	{
		action_pending = false;
		return;
	}
	if (InterruptHoldoffCount || CritSectionCount)
		return;
	action_pending = false;
	MPGetObservation(&observation);
	now = MPMonotonicMillis();
	slot = &actions->slots[MyProcNumber];
	SpinLockAcquire(&actions->lock);
	action = slot->action;
	serial = slot->serial;
	if (observation.mode != MP_MODE_ENFORCE || now > slot->deadline ||
		(slot->requires_pressure && !pressure_is_current(&observation, now)) ||
		!slot->candidate.eligible || !same_session(&slot->target, &slot->candidate.id) ||
		(slot->statement_bound &&
		 (!slot->candidate.active || slot->target.statement != slot->candidate.id.statement)))
		action = MP_ACTION_NONE;
	if (action == MP_ACTION_CANCEL && QueryCancelHoldoffCount)
	{
		SpinLockRelease(&actions->lock);
		action_pending = true;
		InterruptPending = true;
		return;
	}
	SpinLockRelease(&actions->lock);
	/* Do not invoke an adapter while holding a spinlock. */
	if (action != MP_ACTION_NONE && memory_protection_action_safe_hook &&
		!memory_protection_action_safe_hook())
	{
		action_pending = true;
		InterruptPending = true;
		return;
	}
	MPGetObservation(&observation);
	now = MPMonotonicMillis();
	SpinLockAcquire(&actions->lock);
	if (slot->serial != serial)
	{
		SpinLockRelease(&actions->lock);
		action_pending = true;
		InterruptPending = true;
		return;
	}
	if (slot->action != action || now > slot->deadline ||
		(slot->requires_pressure && !pressure_is_current(&observation, now)) ||
		observation.mode != MP_MODE_ENFORCE || now < observation.heartbeat_ms ||
		now - observation.heartbeat_ms > MP_SAMPLE_MAX_AGE_MS ||
		!same_session(&slot->target, &slot->candidate.id) ||
		(slot->statement_bound && (!slot->candidate.active ||
								   slot->target.statement != slot->candidate.id.statement)))
		action = MP_ACTION_NONE;
	slot->action = MP_ACTION_NONE;
	slot->candidate.pending = false;
	SpinLockRelease(&actions->lock);
	if (action != MP_ACTION_NONE)
	{
		LockErrorCleanup();
		ereport(action == MP_ACTION_TERMINATE ? FATAL : ERROR,
				(errcode(action == MP_ACTION_TERMINATE ? ERRCODE_ADMIN_SHUTDOWN : ERRCODE_QUERY_CANCELED),
				 errmsg(action == MP_ACTION_TERMINATE ?
						"terminating connection due to instance memory pressure" :
						"canceling statement due to instance memory pressure")));
	}
}

int
MPChooseCandidate(const MPCandidate *c, int count, uint64 min_bytes, uint64 now)
{
	int			best = -1;
	int			i;

	for (i = 0; i < count; i++)
	{
		bool		idle;
		bool		best_idle;

		if (!c[i].eligible || c[i].pending || !c[i].memory_valid ||
			c[i].private_bytes < min_bytes || c[i].memory_sample_ms > now ||
			now - c[i].memory_sample_ms > MP_SAMPLE_MAX_AGE_MS)
			continue;
		idle = !c[i].active && !c[i].in_transaction;
		best_idle = best >= 0 && !c[best].active && !c[best].in_transaction;
		if (best < 0 || (idle && !best_idle) ||
			(idle == best_idle && c[i].private_bytes > c[best].private_bytes))
			best = i;
	}
	return best;
}

/* Bounded worker sampling adds a lower-bound group estimate, never shared RSS. */
static uint64
worker_private_bytes(const MPIdentity *leader, int budget)
{
	ProcNumber	cursor;
	uint64		total = 0;
	int			visited;

	SpinLockAcquire(&actions->lock);
	cursor = actions->slots[leader->proc_number].worker_cursor;
	SpinLockRelease(&actions->lock);
	for (visited = 0; visited < MaxBackends && budget > 0; visited++)
	{
		MPCandidate before;
		MPCandidate after;
		ProcNumber	index = cursor;
		uint64		bytes;

		cursor = (cursor + 1) % MaxBackends;
		if (!MPReadCandidate(index, &before) ||
			!same_session(leader, &before.leader) ||
			leader->statement != before.leader.statement)
			continue;
		budget--;
		if (MPReadPrivateBytes(before.id.pid, before.start_ticks, &bytes) &&
			MPReadCandidate(index, &after) && same_session(&before.id, &after.id) &&
			same_session(leader, &after.leader) &&
			leader->statement == after.leader.statement)
			total = bytes > PG_UINT64_MAX - total ? PG_UINT64_MAX : total + bytes;
	}
	SpinLockAcquire(&actions->lock);
	if (same_session(leader, &actions->slots[leader->proc_number].candidate.id))
		actions->slots[leader->proc_number].worker_cursor = cursor;
	SpinLockRelease(&actions->lock);
	return total;
}

bool
MPSelectVictim(uint64 min_bytes, MPCandidate *out)
{
	static ProcNumber cursor;
	MPCandidate c[8];
	int			n = 0;
	int			i;
	int			visited;
	int			best;

	for (visited = 0; visited < MaxBackends && n < lengthof(c); visited++)
	{
		MPCandidate before;
		MPCandidate after;
		ProcNumber	index = cursor;
		uint64		bytes;

		cursor = (cursor + 1) % MaxBackends;
		if (!MPReadCandidate(index, &before) || !before.eligible || before.pending)
			continue;
		/* Limit expensive procfs reads even when memory data is unavailable. */
		c[n] = before;
		c[n].memory_sample_ms = MPMonotonicMillis();
		c[n].memory_valid = MPReadPrivateBytes(before.id.pid, before.start_ticks, &bytes);
		c[n].private_bytes = bytes;
		if (!MPReadCandidate(index, &after) || !same_session(&before.id, &after.id) ||
			before.id.statement != after.id.statement || before.active != after.active)
			c[n].memory_valid = false;
		n++;
	}
	/* Fair per-leader shares, at most eight worker reads across this batch. */
	for (i = 0; i < n; i++)
	{
		MPCandidate after;
		uint64		workers;

		if (!c[i].memory_valid)
			continue;
		workers = worker_private_bytes(&c[i].id, lengthof(c) / n);
		c[i].private_bytes = workers > PG_UINT64_MAX - c[i].private_bytes ?
			PG_UINT64_MAX : c[i].private_bytes + workers;
		if (!MPReadCandidate(c[i].id.proc_number, &after) ||
			!same_session(&c[i].id, &after.id) ||
			c[i].id.statement != after.id.statement || c[i].active != after.active)
			c[i].memory_valid = false;
	}
	best = MPChooseCandidate(c, n, min_bytes, MPMonotonicMillis());
	if (best < 0)
		return false;
	*out = c[best];
	return true;
}

void
MPGetActionCounts(uint64 *cancel, uint64 *terminate)
{
	SpinLockAcquire(&actions->lock);
	*cancel = actions->cancel_requests;
	*terminate = actions->terminate_requests;
	SpinLockRelease(&actions->lock);
}

static void
revoke_action(const MPIdentity *id)
{
	MPActionSlot *slot;

	if (id->pid == 0 || id->proc_number < 0 || id->proc_number >= MaxBackends)
		return;
	slot = &actions->slots[id->proc_number];
	SpinLockAcquire(&actions->lock);
	if (same_session(&slot->target, id) && slot->target.statement == id->statement)
	{
		slot->action = MP_ACTION_NONE;
		slot->serial = ++actions->next_action;
		slot->candidate.pending = false;
	}
	SpinLockRelease(&actions->lock);
}

void
MPProtectionTick(const MPObservation *o)
{
	static MPIdentity inflight;
	static MPAction action;
	static uint64 sent_ms;
	static uint64 last_sequence;
	static uint64 cooldown_until;
	static uint64 round_usage;
	static unsigned exits;
	uint64		now = MPMonotonicMillis();
	uint64		remaining;
	uint64		min_bytes;
	MPCandidate c;

	if (o->mode != MP_MODE_ENFORCE || !o->sample.valid ||
		!o->sample.boundary_trusted || o->sample.sampled_at_ms > now ||
		now - o->sample.sampled_at_ms > MP_SAMPLE_MAX_AGE_MS ||
		(o->state != MP_PRESSURE && o->state != MP_CRITICAL))
	{
		revoke_action(&inflight);
		memset(&inflight, 0, sizeof(inflight));
		action = MP_ACTION_NONE;
		exits = 0;
		return;
	}
	if (o->sample.sample_sequence == last_sequence)
		return;
	last_sequence = o->sample.sample_sequence;
	if (now < cooldown_until)
		return;
	remaining = o->sample.current_bytes >= o->sample.limit_bytes ? 0 :
		o->sample.limit_bytes - o->sample.current_bytes;
	if (inflight.pid)
	{
		bool		live = MPReadCandidate(inflight.proc_number, &c) &&
			same_session(&inflight, &c.id);

		if (!live)
		{
			if (++exits >= 3)
			{
				uint64		min_progress = Max(UINT64CONST(16) * 1024 * 1024, o->headroom_bytes / 64);

				if (o->sample.current_bytes >= round_usage ||
					round_usage - o->sample.current_bytes < min_progress)
				{
					cooldown_until = now + 1000;
					ereport(LOG, (errmsg("memory protection paused after three exits without net memory reduction")));
				}
				exits = 0;
			}
		}
		else if (action == MP_ACTION_CANCEL && c.active &&
				 c.id.statement == inflight.statement)
		{
			if (now - sent_ms < 200 && remaining > o->headroom_bytes / 2)
				return;
			if (queue_action(&inflight, MP_ACTION_TERMINATE, true, true))
			{
				action = MP_ACTION_TERMINATE;
				sent_ms = MPMonotonicMillis();
				return;
			}
		}
		else if (action == MP_ACTION_TERMINATE && now - sent_ms < 1000)
			return;
		revoke_action(&inflight);
		memset(&inflight, 0, sizeof(inflight));
		action = MP_ACTION_NONE;
		if (now < cooldown_until)
			return;
	}
	min_bytes = Max(UINT64CONST(64) * 1024 * 1024, o->headroom_bytes / 16);
	if (!MPSelectVictim(min_bytes, &c))
		return;
	action = (!c.active || remaining <= o->headroom_bytes / 2) ?
		MP_ACTION_TERMINATE : MP_ACTION_CANCEL;
	/* Procfs reads may have taken longer than the sample validity window. */
	if (!pressure_is_current(o, MPMonotonicMillis()))
		return;
	if (queue_action(&c.id, action, action == MP_ACTION_CANCEL, true))
	{
		inflight = c.id;
		sent_ms = MPMonotonicMillis();
		if (exits == 0)
			round_usage = o->sample.current_bytes;
		ereport(LOG, (errmsg("memory protection requested %s for PID %d",
							 action == MP_ACTION_CANCEL ? "query cancellation" : "connection termination",
							 (int) c.id.pid),
					  errdetail("Instance usage: " UINT64_FORMAT " bytes; candidate private memory estimate: " UINT64_FORMAT " bytes.",
								o->sample.current_bytes, c.private_bytes)));
	}
}
