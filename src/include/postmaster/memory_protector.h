/*-------------------------------------------------------------------------
 * memory_protector.h -- instance memory pressure observation
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#ifndef MEMORY_PROTECTOR_H
#define MEMORY_PROTECTOR_H

#include "storage/procnumber.h"

#define MP_SAMPLE_MAX_AGE_MS 300

typedef enum MPState
{
	MP_NORMAL = 0,
	MP_PRESSURE,
	MP_CRITICAL,
	MP_STALE
} MPState;

typedef struct MPPressureSample
{
	uint64		limit_bytes;
	uint64		current_bytes;
	uint64		sampled_at_ms;
	uint64		sample_sequence;
	uint64		config_generation;
	bool		valid;
	bool		boundary_trusted;
} MPPressureSample;

/* State belongs to one observer.  Initialize with zeroes before first use. */
typedef struct MPController
{
	MPState		state;
	uint64		last_sequence;
	uint64		config_generation;
	uint64		limit_bytes;
	uint64		headroom_bytes;
	unsigned int pressure_samples;
	unsigned int recovery_samples;
	bool		initialized;
} MPController;

extern MPState MPAdvance(MPController *ctl, const MPPressureSample *sample,
						 uint64 headroom_bytes, uint64 now_ms);
extern bool MPParseBytes(const char *text, uint64 *value);
extern bool MPParseCgroup(const char *text, char *path, Size capacity);
extern bool MPResolveMount(const char *text, const char *membership,
						   char *path, Size capacity);

/* Enforcement requires a fresh, trusted instance boundary. */
typedef enum MPMode
{
	MP_MODE_OFF, MP_MODE_OBSERVE, MP_MODE_ENFORCE
} MPMode;
typedef struct MPObservation
{
	MPPressureSample sample;
	MPState		state;
	int			mode;
	int			observer_pid;
	uint64		heartbeat_ms;
	uint64		headroom_bytes;
	char		reason[64];
} MPObservation;
extern PGDLLIMPORT int memory_protection_mode;
extern PGDLLIMPORT int memory_protection_headroom;
extern Size MPShmemSize(void);
extern void MPShmemInit(void);
extern void MPRegisterObserver(void);
extern PGDLLEXPORT pg_noreturn void MemoryProtectorMain(Datum main_arg);
extern void MPGetObservation(MPObservation *out);
extern uint64 MPMonotonicMillis(void);
extern bool MPReadPressure(MPPressureSample *out, char *reason, Size capacity);

typedef enum MPAction
{
	MP_ACTION_NONE, MP_ACTION_CANCEL, MP_ACTION_TERMINATE
} MPAction;
typedef struct MPIdentity
{
	uint64		epoch;
	uint64		generation;
	uint64		statement;
	ProcNumber	proc_number;
	pid_t		pid;
} MPIdentity;
typedef struct MPCandidate
{
	MPIdentity	id;
	MPIdentity	leader;
	uint64		private_bytes;
	uint64		memory_sample_ms;
	uint64		start_ticks;
	bool		eligible;
	bool		active;
	bool		in_transaction;
	bool		pending;
	bool		memory_valid;
} MPCandidate;
extern Size MPActionsShmemSize(void);
extern void MPActionsShmemInit(void);
extern void MPBackendRegister(bool privileged);
extern void MPParallelRegister(const MPIdentity *leader);
extern void MPStatementBegin(void);
extern void MPStatementEnd(void);
extern void MPBackendIdle(bool in_transaction);
extern void MPHandleInterrupt(void);
extern void MPProcessInterrupt(void);
extern bool MPReadCandidate(ProcNumber slot, MPCandidate *out);

/* Raw delivery primitives for explicit tests; controller also binds pressure. */
extern bool MPQueueEscalation(const MPIdentity *id);
extern bool MPQueueAction(const MPIdentity *id, MPAction action);
extern int	MPChooseCandidate(const MPCandidate *c, int count, uint64 min_bytes, uint64 now);
extern bool MPSelectVictim(uint64 min_bytes, MPCandidate *out);
extern void MPGetActionCounts(uint64 *cancel, uint64 *terminate);
extern void MPProtectionTick(const MPObservation *observation);
extern bool MPParsePrivateBytes(const char *text, uint64 *bytes);
extern bool MPReadProcessStart(pid_t pid, uint64 *ticks);
extern bool MPReadPrivateBytes(pid_t pid, uint64 ticks, uint64 *bytes);

/* Storage adapters can defer actions at unsafe protocol boundaries. */
typedef bool (*MPActionSafeHook) (void);
extern PGDLLIMPORT MPActionSafeHook memory_protection_action_safe_hook;
#endif
