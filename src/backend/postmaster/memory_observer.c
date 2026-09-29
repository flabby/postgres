/*-------------------------------------------------------------------------
 * memory_observer.c -- background pressure sampling and bounded protection loop
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/memory_protector.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

int			memory_protection_mode = MP_MODE_OFF;
int			memory_protection_headroom = 0; /* kB; zero disables pressure
											 * classification */

typedef struct MPShared
{
	slock_t		lock;
	MPObservation observation;
} MPShared;
static MPShared *shared;

uint64
MPMonotonicMillis(void)
{
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);
	return (uint64) INSTR_TIME_GET_MILLISEC(now);
}

Size
MPShmemSize(void)
{
	return sizeof(MPShared);
}

void
MPShmemInit(void)
{
	bool		found;

	shared = ShmemInitStruct("Memory protector observation", sizeof(MPShared), &found);
	if (!found)
	{
		memset(shared, 0, sizeof(*shared));
		SpinLockInit(&shared->lock);
		shared->observation.state = MP_STALE;
		strlcpy(shared->observation.reason, "observer_not_started",
				sizeof(shared->observation.reason));
	}
}

void
MPGetObservation(MPObservation *out)
{
	SpinLockAcquire(&shared->lock);
	memcpy(out, &shared->observation, sizeof(*out));
	SpinLockRelease(&shared->lock);
}

void
MPRegisterObserver(void)
{
	BackgroundWorker worker = {0};

	strlcpy(worker.bgw_name, "memory pressure observer", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "memory pressure observer", BGW_MAXLEN);
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 1;
	strlcpy(worker.bgw_library_name, "postgres", MAXPGPATH);
	strlcpy(worker.bgw_function_name, "MemoryProtectorMain", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

void
MemoryProtectorMain(Datum main_arg)
{
	MPController controller = {0};
	MPObservation observation = {0};
	uint64		generation = 1;
	uint64		sequence = 0;
	uint32		wait_event;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	wait_event = WaitEventExtensionNew("MemoryPressureObserver");

	for (;;)
	{
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			generation++;
		}
		memset(&observation, 0, sizeof(observation));
		observation.mode = memory_protection_mode;
		observation.observer_pid = MyProcPid;
		observation.headroom_bytes = (uint64) memory_protection_headroom * 1024;
		observation.sample.sampled_at_ms = MPMonotonicMillis();
		if (memory_protection_mode != MP_MODE_OFF)
			MPReadPressure(&observation.sample, observation.reason,
						   sizeof(observation.reason));
		else
			strlcpy(observation.reason, "disabled", sizeof(observation.reason));
		observation.heartbeat_ms = MPMonotonicMillis();
		observation.sample.sample_sequence = ++sequence;
		observation.sample.config_generation = generation;
		observation.state = MPAdvance(&controller, &observation.sample,
									  observation.headroom_bytes,
									  observation.heartbeat_ms);
		SpinLockAcquire(&shared->lock);
		memcpy(&shared->observation, &observation, sizeof(observation));
		SpinLockRelease(&shared->lock);
		MPProtectionTick(&observation);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 memory_protection_mode == MP_MODE_OFF ? 1000L : 100L,
						 wait_event);
	}
}
