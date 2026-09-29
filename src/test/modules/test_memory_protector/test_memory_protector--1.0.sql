CREATE FUNCTION test_mp_policy() RETURNS boolean
AS 'MODULE_PATHNAME', 'test_mp_policy' LANGUAGE C STRICT;
CREATE FUNCTION test_mp_pressure_parser() RETURNS boolean
AS 'MODULE_PATHNAME', 'test_mp_pressure_parser' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_policy() FROM PUBLIC;
REVOKE ALL ON FUNCTION test_mp_pressure_parser() FROM PUBLIC;
CREATE FUNCTION test_mp_observer() RETURNS boolean
AS 'MODULE_PATHNAME', 'test_mp_observer' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_observer() FROM PUBLIC;
CREATE FUNCTION test_mp_wait_for_mode(text) RETURNS boolean
AS 'MODULE_PATHNAME', 'test_mp_wait_for_mode' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_wait_for_mode(text) FROM PUBLIC;
CREATE FUNCTION test_mp_wait_for_restart() RETURNS boolean
AS 'MODULE_PATHNAME', 'test_mp_wait_for_restart' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_wait_for_restart() FROM PUBLIC;
CREATE FUNCTION test_mp_selection() RETURNS boolean AS 'MODULE_PATHNAME', 'test_mp_selection' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_selection() FROM PUBLIC;
CREATE FUNCTION test_mp_snapshot(int) RETURNS text AS 'MODULE_PATHNAME', 'test_mp_snapshot' LANGUAGE C STRICT;
CREATE FUNCTION test_mp_dispatch(int,bigint,bigint,text) RETURNS boolean AS 'MODULE_PATHNAME', 'test_mp_dispatch' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_snapshot(int), test_mp_dispatch(int,bigint,bigint,text) FROM PUBLIC;
CREATE FUNCTION test_mp_defer(int) RETURNS void AS 'MODULE_PATHNAME', 'test_mp_defer' LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION test_mp_defer(int) FROM PUBLIC;
