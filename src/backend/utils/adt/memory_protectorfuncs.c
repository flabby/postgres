/*-------------------------------------------------------------------------
 * memory_protectorfuncs.c -- bounded memory observer status
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "catalog/pg_authid_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/memory_protector.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

Datum
pg_memory_protection_status(PG_FUNCTION_ARGS)
{
	MPObservation o;
	TupleDesc	desc;
	Datum		values[12];
	bool		nulls[12] = {false};
	uint64		now;
	uint64		age;
	uint64		cancel_requests,
				terminate_requests;
	const char *states[] = {"normal", "pressure", "critical", "stale"};

	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to read memory protection status")));
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	MPGetObservation(&o);
	now = MPMonotonicMillis();
	age = now >= o.sample.sampled_at_ms ? now - o.sample.sampled_at_ms : UINT64_MAX;
	if (age > MP_SAMPLE_MAX_AGE_MS)
		o.state = MP_STALE;
	values[0] = CStringGetTextDatum(o.mode == MP_MODE_ENFORCE ? "enforce" : (o.mode == MP_MODE_OBSERVE ? "observe" : "off"));
	values[1] = CStringGetTextDatum(states[o.state]);
	values[2] = Int32GetDatum(o.observer_pid);
	values[3] = Int64GetDatum((int64) o.sample.limit_bytes);
	values[4] = Int64GetDatum((int64) o.sample.current_bytes);
	values[5] = BoolGetDatum(o.sample.valid && age <= MP_SAMPLE_MAX_AGE_MS);
	values[6] = CStringGetTextDatum(o.reason);
	values[7] = Int64GetDatum((int64) age);
	values[8] = Int64GetDatum((int64) o.headroom_bytes);
	MPGetActionCounts(&cancel_requests, &terminate_requests);
	values[9] = BoolGetDatum(o.sample.boundary_trusted && o.sample.valid && age <= MP_SAMPLE_MAX_AGE_MS);
	values[10] = Int64GetDatum(cancel_requests);
	values[11] = Int64GetDatum(terminate_requests);
	nulls[2] = o.observer_pid == 0;
	nulls[3] = !o.sample.valid || o.sample.limit_bytes > PG_INT64_MAX;
	nulls[4] = !o.sample.valid || o.sample.current_bytes > PG_INT64_MAX;
	nulls[7] = o.sample.sampled_at_ms == 0 || age > PG_INT64_MAX;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}
