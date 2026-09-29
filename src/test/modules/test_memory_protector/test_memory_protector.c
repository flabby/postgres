/* Tests exercise policy boundaries and untrusted kernel text, without signals. */
#include "postgres.h"
#include <signal.h>
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "postmaster/memory_protector.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_mp_wait_for_restart);
PG_FUNCTION_INFO_V1(test_mp_wait_for_mode);
PG_FUNCTION_INFO_V1(test_mp_observer);
PG_FUNCTION_INFO_V1(test_mp_policy);
PG_FUNCTION_INFO_V1(test_mp_pressure_parser);
#define CHECK(c, msg) do { if (!(c)) elog(ERROR, "memory protector: %s", msg); } while (0)
#define GIB UINT64CONST(1073741824)

static MPState
step(MPController *ctl, MPPressureSample *s, uint64 used, uint64 seq)
{
	s->current_bytes = used;
	s->sample_sequence = seq;
	return MPAdvance(ctl, s, 4 * GIB, 1000);
}

Datum
test_mp_policy(PG_FUNCTION_ARGS)
{
	MPController c = {0};
	MPPressureSample s = {0};

	s.valid = true;
	s.limit_bytes = 32 * GIB;
	s.sampled_at_ms = 1000;
	s.config_generation = 1;
	CHECK(step(&c, &s, 27 * GIB, 1) == MP_NORMAL, "normal below threshold");
	CHECK(step(&c, &s, 28 * GIB, 2) == MP_NORMAL, "one pressure sample is insufficient");
	CHECK(step(&c, &s, 28 * GIB, 2) == MP_NORMAL, "duplicate sample must not count");
	CHECK(step(&c, &s, 28 * GIB, 3) == MP_PRESSURE, "second pressure sample");
	CHECK(step(&c, &s, 30 * GIB, 4) == MP_CRITICAL, "critical is immediate");
	CHECK(step(&c, &s, 26 * GIB, 5) == MP_CRITICAL, "first recovery sample");
	CHECK(step(&c, &s, 26 * GIB, 6) == MP_CRITICAL, "second recovery sample");
	CHECK(step(&c, &s, 26 * GIB, 7) == MP_NORMAL, "third recovery sample");
	CHECK(step(&c, &s, 33 * GIB, 8) == MP_CRITICAL, "usage beyond limit does not underflow");
	CHECK(MPAdvance(&c, &s, 4 * GIB, 1301) == MP_STALE, "duplicate can expire");
	CHECK(step(&c, &s, 20 * GIB, 9) == MP_NORMAL, "fresh normal sample leaves stale");
	CHECK(MPAdvance(&c, &s, 0, 1000) == MP_STALE, "zero headroom invalid");
	CHECK(MPAdvance(&c, &s, 24 * GIB, 1000) == MP_STALE, "unreachable recovery invalid");
	CHECK(MPAdvance(&c, &s, UINT64_MAX, 1000) == MP_STALE, "headroom overflow rejected");
	s.valid = false;
	CHECK(step(&c, &s, 31 * GIB, 10) == MP_STALE, "invalid sample cannot trigger");
	s.valid = true;
	s.sampled_at_ms = 1001;
	CHECK(step(&c, &s, 31 * GIB, 11) == MP_STALE, "future sample rejected");
	s.sampled_at_ms = 1000;
	CHECK(step(&c, &s, 28 * GIB, 12) == MP_NORMAL, "restart consecutive count");
	s.config_generation++;
	CHECK(step(&c, &s, 28 * GIB, 13) == MP_NORMAL, "new config resets count");
	CHECK(step(&c, &s, 28 * GIB, 14) == MP_PRESSURE, "new config second sample");
	CHECK(step(&c, &s, 28 * GIB, 13) == MP_STALE, "out of order sample rejected");
	PG_RETURN_BOOL(true);
}

Datum
test_mp_pressure_parser(PG_FUNCTION_ARGS)
{
	uint64		v;
	char		path[MAXPGPATH];

	CHECK(MPParseBytes("34359738368\n", &v) && v == 32 * GIB, "decimal bytes");
	CHECK(MPParseBytes("0\n", &v) && v == 0, "zero is valid usage");
	CHECK(MPParseBytes("18446744073709551615\n", &v) && v == UINT64_MAX, "uint64 boundary");
	CHECK(!MPParseBytes("18446744073709551616\n", &v), "overflow");
	CHECK(!MPParseBytes("max\n", &v), "unbounded is not zero");
	CHECK(!MPParseBytes("-1", &v), "negative");
	CHECK(!MPParseBytes("+1", &v), "sign");
	CHECK(!MPParseBytes("12garbage", &v), "suffix");
	CHECK(!MPParseBytes("", &v), "empty");
	CHECK(!MPParseBytes("1\n2", &v), "multiple values");
	CHECK(MPParseCgroup("9:cpu:/old\n0::/instances/a\n", path, sizeof(path)) &&
		  strcmp(path, "/instances/a") == 0, "unified membership");
	CHECK(!MPParseCgroup("0::/a\n0::/b\n", path, sizeof(path)), "ambiguous membership");
	CHECK(!MPParseCgroup("0::/a/../b\n", path, sizeof(path)), "traversal");
	CHECK(!MPParseCgroup("0::/toolong\n", path, 4), "truncation");
	CHECK(MPResolveMount("31 22 0:28 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n",
						 "/instances/a", path, sizeof(path)) &&
		  strcmp(path, "/sys/fs/cgroup/instances/a") == 0, "root mount");
	CHECK(MPResolveMount("31 22 0:28 /instances /cg\\040root rw shared:1 - cgroup2 cgroup rw\n",
						 "/instances/a", path, sizeof(path)) &&
		  strcmp(path, "/cg root/a") == 0, "subtree and escape");
	CHECK(!MPResolveMount("31 22 0:28 /instances /cg rw - cgroup2 cgroup rw\n",
						  "/instances-other/a", path, sizeof(path)), "component boundary");
	CHECK(!MPResolveMount("31 22 0:28 / /cg rw - cgroup cgroup rw\n",
						  "/a", path, sizeof(path)), "v1 unsupported");
	PG_RETURN_BOOL(true);
}

/* Production observer must publish a heartbeat without sending any actions. */
Datum
test_mp_observer(PG_FUNCTION_ARGS)
{
	MPObservation observation;
	int			i;

	for (i = 0; i < 500; i++)
	{
		MPGetObservation(&observation);
		if (observation.observer_pid > 0 && observation.sample.sample_sequence > 0)
			break;
		pg_usleep(10000L);
		CHECK_FOR_INTERRUPTS();
	}
	CHECK(observation.observer_pid > 0, "observer did not start");
	CHECK(observation.sample.sample_sequence > 0, "observer did not publish");
	CHECK(observation.mode == MP_MODE_OBSERVE, "observer is not in observe mode");
#ifndef __linux__
	CHECK(!observation.sample.valid, "non-Linux sample must be invalid");
	CHECK(observation.state == MP_STALE, "unsupported platform must be stale");
#endif
	PG_RETURN_BOOL(true);
}

Datum
test_mp_wait_for_mode(PG_FUNCTION_ARGS)
{
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			expected;
	MPObservation observation;
	int			i;

	if (strcmp(mode, "off") == 0)
		expected = MP_MODE_OFF;
	else if (strcmp(mode, "observe") == 0)
		expected = MP_MODE_OBSERVE;
	else
		elog(ERROR, "unsupported test mode");
	for (i = 0; i < 500; i++)
	{
		MPGetObservation(&observation);
		if (observation.mode == expected)
			PG_RETURN_BOOL(true);
		pg_usleep(10000L);
		CHECK_FOR_INTERRUPTS();
	}
	elog(ERROR, "observer did not reload mode");
	PG_RETURN_BOOL(false);
}

Datum
test_mp_wait_for_restart(PG_FUNCTION_ARGS)
{
	int			old_pid;
	MPObservation observation;
	int			i;

	MPGetObservation(&observation);
	old_pid = observation.observer_pid;
	CHECK(old_pid > 1 && old_pid != MyProcPid, "missing observer PID");
	CHECK(MPMonotonicMillis() - observation.heartbeat_ms <= 300,
		  "observer heartbeat stale before restart test");
	CHECK(kill(old_pid, SIGTERM) == 0, "could not stop test observer");
	for (i = 0; i < 600; i++)
	{
		MPGetObservation(&observation);
		if (observation.observer_pid > 0 && observation.observer_pid != old_pid &&
			observation.sample.sample_sequence > 0)
			PG_RETURN_BOOL(true);
		pg_usleep(10000L);
		CHECK_FOR_INTERRUPTS();
	}
	elog(ERROR, "observer did not restart");
	PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(test_mp_selection);
Datum
test_mp_selection(PG_FUNCTION_ARGS)
{
	uint64		bytes;
	MPCandidate c[4] = {0};
	int			i;

	for (i = 0; i < 4; i++)
	{
		c[i].eligible = true;
		c[i].memory_valid = true;
		c[i].memory_sample_ms = 1000;
		c[i].active = true;
		c[i].id.pid = i + 100;
	}
	CHECK(MPParsePrivateBytes("Private_Clean: 16 kB\nPrivate_Dirty: 32 kB\nPrivate_Hugetlb: 64 kB\n", &bytes) && bytes == 114688, "private pages counted once");
	CHECK(!MPParsePrivateBytes("Private_Clean: 1 kB\n", &bytes), "missing dirty invalid");
	CHECK(!MPParsePrivateBytes("Private_Clean: 1 kB\nPrivate_Dirty: 18446744073709551615 kB\n", &bytes), "byte conversion overflow");
	CHECK(!MPParsePrivateBytes("Private_Clean: 1 MB\nPrivate_Dirty: 1 kB\n", &bytes), "unexpected unit invalid");
	c[0].private_bytes = 4 * GIB;
	c[1].private_bytes = GIB / 2;
	c[1].active = false;
	c[2].private_bytes = GIB;
	c[2].active = false;
	c[2].in_transaction = true;
	c[3].private_bytes = 8 * GIB;
	c[3].eligible = false;
	CHECK(MPChooseCandidate(c, 4, GIB / 16, 1000) == 1, "large true idle first");
	CHECK(MPChooseCandidate(c, 4, GIB, 1000) == 0, "small idle skipped; transaction not cheap idle");
	c[0].pending = true;
	c[1].memory_valid = false;
	CHECK(MPChooseCandidate(c, 4, GIB / 16, 1000) == 2, "pending and unknown excluded");
	CHECK(MPChooseCandidate(c, 4, GIB / 16, 1301) == -1, "stale memory excluded");
	CHECK(MPChooseCandidate(c, 4, GIB / 16, 999) == -1, "future memory excluded");
	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(test_mp_snapshot);
PG_FUNCTION_INFO_V1(test_mp_dispatch);
Datum
test_mp_snapshot(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	int			i;
	MPCandidate c;

	for (i = 0; i < MaxBackends; i++)
		if (MPReadCandidate(i, &c) && c.id.pid == pid)
			PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT "," UINT64_FORMAT ",%d,%d",
													  c.id.generation, c.id.statement, c.active, c.eligible)));
	PG_RETURN_NULL();
}
Datum
test_mp_dispatch(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	uint64		generation = PG_GETARG_INT64(1);
	uint64		statement = PG_GETARG_INT64(2);
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(3));
	MPAction	action;
	MPCandidate c;
	int			i;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("superuser required")));
	if (strcmp(name, "cancel") == 0)
		action = MP_ACTION_CANCEL;
	else if (strcmp(name, "terminate") == 0 || strcmp(name, "escalate") == 0)
		action = MP_ACTION_TERMINATE;
	else
		elog(ERROR, "invalid test action");
	for (i = 0; i < MaxBackends; i++)
		if (MPReadCandidate(i, &c) && c.id.pid == pid)
		{
			c.id.generation = generation;
			c.id.statement = statement;
			PG_RETURN_BOOL(strcmp(name, "escalate") == 0 ? MPQueueEscalation(&c.id) : MPQueueAction(&c.id, action));
		}
	PG_RETURN_BOOL(false);
}

/* Test-only storage adapter: delays action acceptance without touching PG data. */
static uint64 test_safe_after;
static bool
test_storage_safe(void)
{
	return MPMonotonicMillis() >= test_safe_after;
}

PG_FUNCTION_INFO_V1(test_mp_defer);
Datum
test_mp_defer(PG_FUNCTION_ARGS)
{
	int			ms = PG_GETARG_INT32(0);

	if (ms < 0 || ms > 5000)
		elog(ERROR, "test deferral out of range");
	test_safe_after = MPMonotonicMillis() + ms;
	memory_protection_action_safe_hook = test_storage_safe;
	PG_RETURN_VOID();
}
