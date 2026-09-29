#!/usr/bin/env python3
"""Isolated libpq integration tests; stdlib only, no workload/OOM injection.
Usage: python3 test_actions.py /absolute/path/to/test/installation
"""
import ctypes as C
import pathlib
import subprocess
import sys
import tempfile
import time

prefix = pathlib.Path(sys.argv[1]).resolve()
lib = C.CDLL(str(prefix / "lib" / ("libpq.dylib" if sys.platform == "darwin" else "libpq.so")))
for name, result, args in [
    ("PQconnectdb", C.c_void_p, [C.c_char_p]), ("PQstatus", C.c_int, [C.c_void_p]),
    ("PQbackendPID", C.c_int, [C.c_void_p]), ("PQfinish", None, [C.c_void_p]),
    ("PQexec", C.c_void_p, [C.c_void_p, C.c_char_p]),
    ("PQsendQuery", C.c_int, [C.c_void_p, C.c_char_p]),
    ("PQsendQueryParams", C.c_int, [C.c_void_p,C.c_char_p,C.c_int,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.c_int]),
    ("PQconsumeInput", C.c_int, [C.c_void_p]), ("PQisBusy", C.c_int, [C.c_void_p]),
    ("PQgetResult", C.c_void_p, [C.c_void_p]), ("PQresultStatus", C.c_int, [C.c_void_p]),
    ("PQntuples", C.c_int, [C.c_void_p]), ("PQnfields", C.c_int, [C.c_void_p]),
    ("PQgetvalue", C.c_char_p, [C.c_void_p,C.c_int,C.c_int]),
    ("PQresultErrorField", C.c_char_p, [C.c_void_p,C.c_int]),
    ("PQresultErrorMessage", C.c_char_p, [C.c_void_p]), ("PQclear", None, [C.c_void_p])]:
    f=getattr(lib,name);f.restype=result;f.argtypes=args

connections=[]
class Conn:
    def __init__(self, sock, user):
        self.p=lib.PQconnectdb(f"host={sock} dbname=postgres user={user} connect_timeout=5".encode())
        assert lib.PQstatus(self.p)==0, "connection failed"
        self.pid=lib.PQbackendPID(self.p);connections.append(self)
    def unpack(self,res):
        assert res, "no result"
        code=lib.PQresultErrorField(res,ord('C'))
        if not code and lib.PQresultStatus(res) not in (1,2):code=b'CONNECTION_FAILURE'
        msg=lib.PQresultErrorMessage(res).decode()
        rows=[[lib.PQgetvalue(res,i,j).decode() for j in range(lib.PQnfields(res))]
              for i in range(lib.PQntuples(res))]
        lib.PQclear(res)
        return code.decode() if code else None, rows, msg
    def query(self,sql):
        code,rows,msg=self.unpack(lib.PQexec(self.p,sql.encode()))
        assert code is None, (sql,code,msg)
        return rows
    def send(self,sql,extended=False):
        if extended:
            ok=lib.PQsendQueryParams(self.p,sql.encode(),0,None,None,None,None,0)
        else:ok=lib.PQsendQuery(self.p,sql.encode())
        assert ok==1
    def finish(self):
        results=[];end=time.monotonic()+10
        while True:
            assert time.monotonic()<end, "query did not finish"
            lib.PQconsumeInput(self.p)
            if lib.PQisBusy(self.p):time.sleep(.01);continue
            res=lib.PQgetResult(self.p)
            if not res:return results
            results.append(self.unpack(res))
    def close(self):
        if self.p:lib.PQfinish(self.p);self.p=None

def wait(fn):
    end=time.monotonic()+5
    while time.monotonic()<end:
        value=fn()
        if value:return value
        time.sleep(.01)
    raise AssertionError("condition timed out")

with tempfile.TemporaryDirectory(prefix="mp-actions-") as temp:
    root=pathlib.Path(temp);data=root/'data';sock=root/'sock';sock.mkdir()
    subprocess.run([str(prefix/'bin/initdb'),'-D',str(data),'--auth=trust','--no-sync'],check=True,stdout=subprocess.DEVNULL)
    with (data/'postgresql.conf').open('a') as f:
        f.write(f"\nlisten_addresses=''\nunix_socket_directories='{sock}'\nmemory_protection_mode='observe'\nmemory_protection_headroom='4GB'\n")
    ctl=[str(prefix/'bin/pg_ctl'),'-D',str(data)]
    subprocess.run(ctl+['-l',str(root/'server.log'),'-w','start'],check=True,stdout=subprocess.DEVNULL)
    try:
        import getpass
        admin=Conn(sock,getpass.getuser());admin.query('CREATE EXTENSION test_memory_protector')
        admin.query('CREATE ROLE mp_client LOGIN')
        admin.query('GRANT EXECUTE ON FUNCTION test_mp_defer(int) TO mp_client')
        def snap(c,active=None,wait_sleep=True):
            def read():
                if active == 1 and wait_sleep and admin.query(f"SELECT count(*) FROM pg_stat_activity WHERE pid={c.pid} AND wait_event='PgSleep'")[0][0] != '1':
                    return None
                rows=admin.query(f'SELECT test_mp_snapshot({c.pid})')
                if not rows[0][0]:return None
                fields=list(map(int,rows[0][0].split(',')))
                return fields if active is None or fields[2]==active else None
            return wait(read)
        def dispatch(c,identity,action):
            g,q=identity[:2]
            return admin.query(f"SELECT test_mp_dispatch({c.pid},{g},{q},'{action}')")[0][0]=='t'
        c=Conn(sock,'mp_client');identity=snap(c,0)
        assert not dispatch(c,identity,'terminate'), 'observe must not dispatch'
        admin.query("ALTER SYSTEM SET memory_protection_mode='enforce'")
        admin.query('SELECT pg_reload_conf()')
        wait(lambda:admin.query("SELECT mode FROM pg_memory_protection_status()")[0][0]=='enforce')
        c.close();c=Conn(sock,'mp_client')
        # Cancel a running query, preserve the connection; simple + extended.
        for extended in (False,True):
            c.send('SELECT pg_sleep(30)',extended);identity=snap(c,1)
            assert dispatch(c,identity,'cancel')
            result=c.finish();assert result[0][0]=='57014', result
            assert 'instance memory pressure' in result[0][2]
            assert c.query('SELECT 42')==[['42']]
        # An old generation or old query must not affect the new query.
        old=identity;c.send('SELECT pg_sleep(30)');current=snap(c,1)
        assert not dispatch(c,old,'cancel')
        assert not dispatch(c,old,'escalate')
        wrong=current.copy();wrong[0]+=1
        assert not dispatch(c,wrong,'terminate')
        assert dispatch(c,current,'cancel');assert c.finish()[0][0]=='57014'
        # A queued cancellation held behind the storage safe point must not
        # reach a later statement; expiry must also discard delayed termination.
        c.query('SELECT test_mp_defer(1500)')
        c.send('SELECT pg_sleep(0.2)');identity=snap(c,1)
        assert dispatch(c,identity,'cancel');assert c.finish()[0][0] is None
        c.send('SELECT pg_sleep(1.6)');assert c.finish()[0][0] is None
        c.query('SELECT test_mp_defer(1500)')
        c.send('SELECT pg_sleep(1.7)');identity=snap(c,1)
        assert dispatch(c,identity,'terminate');assert c.finish()[0][0] is None
        assert c.query('SELECT 5')==[['5']]
        # Escalation belongs to the original statement even before expiry.
        c.query('SELECT test_mp_defer(500)')
        c.send('SELECT pg_sleep(0.2)');identity=snap(c,1)
        assert dispatch(c,identity,'escalate');assert c.finish()[0][0] is None
        c.send('SELECT pg_sleep(0.6)');assert c.finish()[0][0] is None
        # Reloading off revokes an action deferred at a storage safe point.
        c.query('SELECT test_mp_defer(500)')
        c.send('SELECT pg_sleep(0.8)');identity=snap(c,1)
        assert dispatch(c,identity,'terminate')
        admin.query("ALTER SYSTEM SET memory_protection_mode='off'")
        admin.query('SELECT pg_reload_conf()')
        wait(lambda:admin.query("SELECT mode FROM pg_memory_protection_status()")[0][0]=='off')
        assert c.finish()[0][0] is None
        admin.query("ALTER SYSTEM SET memory_protection_mode='enforce'")
        admin.query('SELECT pg_reload_conf()')
        wait(lambda:admin.query("SELECT mode FROM pg_memory_protection_status()")[0][0]=='enforce')
        # Explicit transaction keeps normal failed-transaction semantics.
        c.query('BEGIN');c.send('SELECT pg_sleep(30)');identity=snap(c,1)
        assert dispatch(c,identity,'cancel');assert c.finish()[0][0]=='57014'
        code,_,_=c.unpack(lib.PQexec(c.p,b'SELECT 1'));assert code=='25P02'
        c.query('ROLLBACK');assert c.query('SELECT 1')==[['1']]
        # Superuser session is protected even through the test dispatcher.
        assert not dispatch(admin,snap(admin),'terminate')
        # Terminate idle connection, preserving other sessions and postmaster.
        identity=snap(c,0);assert dispatch(c,identity,'terminate')
        time.sleep(.1)
        code,_,_=c.unpack(lib.PQexec(c.p,b'SELECT 1'));assert code in ('57P01','CONNECTION_FAILURE') and lib.PQstatus(c.p)!=0,code
        assert admin.query('SELECT 7')==[['7']]
        c.close();c=Conn(sock,'mp_client')
        c.send('SELECT pg_sleep(30)');identity=snap(c,1)
        assert dispatch(c,identity,'terminate');assert c.finish()[0][0]=='57P01'
        assert admin.query('SELECT 8')==[['8']]
        # Parallel workers are registered as protected members of a leader.
        admin.query('CREATE TABLE mp_parallel AS SELECT generate_series(1,10000) AS x')
        admin.query('GRANT SELECT ON mp_parallel TO mp_client')
        admin.query("CREATE FUNCTION mp_slow(int) RETURNS int LANGUAGE plpgsql PARALLEL SAFE AS $$BEGIN PERFORM pg_sleep(0.001); RETURN $1; END$$")
        for action in ('cancel','terminate'):
            c=Conn(sock,'mp_client')
            c.query('SET max_parallel_workers_per_gather=2')
            c.query('SET min_parallel_table_scan_size=0')
            c.query('SET parallel_setup_cost=0')
            c.query('SET parallel_tuple_cost=0')
            c.send('SELECT sum(mp_slow(x)) FROM mp_parallel')
            workers=wait(lambda:admin.query(f"SELECT pid FROM pg_stat_activity WHERE leader_pid={c.pid}"))
            for row in workers:
                worker=type('Worker',(),{'pid':int(row[0])})()
                wid=snap(worker)
                assert wid[3]==0, 'parallel worker must be protected'
                assert not dispatch(worker,wid,'terminate')
            identity=snap(c,1,wait_sleep=False)
            assert dispatch(c,identity,action)
            assert c.finish()[0][0]==('57014' if action=='cancel' else '57P01')
            wait(lambda:admin.query(f"SELECT count(*) FROM pg_stat_activity WHERE leader_pid={c.pid}")[0][0]=='0')
            if action=='cancel':
                assert c.query('SELECT 9')==[['9']]
                # Exercise the existing parallel error signal and a message
                # larger than its queue, catching inconsistent signal IDs.
                c.query('SET parallel_leader_participation=off')
                c.send("SELECT (repeat('abcd',5000)||x::text)::int2 FROM mp_parallel")
                assert c.finish()[0][0]=='22P02'
                assert c.query('SELECT 11')==[['11']]
            assert admin.query('SELECT 10')==[['10']]
            c.close()
        print('PASS: observe gating; simple/extended cancellation; stale identity; transaction rollback; protected role; idle/active termination; deferred expiry/revocation; parallel cleanup')
    except BaseException:
        print((root/'server.log').read_text(),file=sys.stderr)
        raise
    finally:
        for c in connections:c.close()
        try:
            subprocess.run(ctl+['-m','fast','-w','stop'],check=True,
                           stdout=subprocess.DEVNULL,timeout=15)
        except subprocess.TimeoutExpired:
            subprocess.run(ctl+['-m','immediate','-w','stop'],check=True,
                           stdout=subprocess.DEVNULL,timeout=15)
