/*-------------------------------------------------------------------------
 * memory_protector.c -- deterministic instance pressure policy
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "postmaster/memory_protector.h"

static MPState
stale(MPController *ctl)
{
	ctl->pressure_samples = 0;
	ctl->recovery_samples = 0;
	ctl->state = MP_STALE;
	return ctl->state;
}

MPState
MPAdvance(MPController *ctl, const MPPressureSample *s,
		  uint64 headroom, uint64 now_ms)
{
	uint64		remaining;

	/* Validate before arithmetic, including the recovery watermark. */
	if (!s->valid || s->limit_bytes == 0 || headroom == 0 ||
		headroom >= s->limit_bytes ||
		headroom / 2 >= s->limit_bytes - headroom ||
		s->sample_sequence == 0 || s->sampled_at_ms > now_ms ||
		now_ms - s->sampled_at_ms > MP_SAMPLE_MAX_AGE_MS)
		return stale(ctl);

	if (!ctl->initialized || ctl->config_generation != s->config_generation ||
		ctl->limit_bytes != s->limit_bytes || ctl->headroom_bytes != headroom)
	{
		memset(ctl, 0, sizeof(*ctl));
		ctl->initialized = true;
		ctl->config_generation = s->config_generation;
		ctl->limit_bytes = s->limit_bytes;
		ctl->headroom_bytes = headroom;
	}
	if (s->sample_sequence < ctl->last_sequence)
		return stale(ctl);
	if (s->sample_sequence == ctl->last_sequence)
		return ctl->state;
	ctl->last_sequence = s->sample_sequence;

	if (ctl->state == MP_STALE)
		ctl->state = MP_NORMAL;
	remaining = s->current_bytes >= s->limit_bytes ? 0 :
		s->limit_bytes - s->current_bytes;
	if (remaining <= headroom / 2)
	{
		ctl->state = MP_CRITICAL;
		ctl->pressure_samples = 0;
		ctl->recovery_samples = 0;
	}
	else if (remaining <= headroom)
	{
		ctl->recovery_samples = 0;
		if (ctl->pressure_samples < 2)
			ctl->pressure_samples++;
		if (ctl->pressure_samples >= 2 && ctl->state == MP_NORMAL)
			ctl->state = MP_PRESSURE;
	}
	else
	{
		ctl->pressure_samples = 0;
		if (remaining >= headroom + headroom / 2)
		{
			if (ctl->recovery_samples < 3)
				ctl->recovery_samples++;
			if (ctl->recovery_samples >= 3)
				ctl->state = MP_NORMAL;
		}
		else
			ctl->recovery_samples = 0;
	}
	return ctl->state;
}
