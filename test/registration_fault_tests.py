#!/usr/bin/env python3
"""Run against the task-owned temporary MySQL container only. Never business DBs."""
import pathlib, subprocess, time
ROOT = pathlib.Path(__file__).resolve().parents[1]
NAME = 'ape-registration-test'

def run(args, **kw):
    return subprocess.run(args, check=True, text=True, **kw)

def mysql(sql, schema='ape_registration_test', ok=True):
    label = run(['docker','inspect','-f','{{index .Config.Labels "purpose"}}',NAME],capture_output=True).stdout.strip()
    if label != 'registration-tests': raise RuntimeError('Refusing a container not owned by these tests')
    r = subprocess.run(['docker','exec','-i','-e','MYSQL_PWD=registration-test-only',NAME,
        'mysql','-uroot','--batch','--skip-column-names',schema], input=sql,text=True,capture_output=True)
    if ok and r.returncode: raise RuntimeError(r.stderr)
    if not ok and not r.returncode: raise RuntimeError('Migration unexpectedly accepted invalid fixture')
    return r

migration = (ROOT/'scripts/migrations/registration_v1.sql').read_text()
mysql(migration) # rerunnable after real registrations
print('PASS migration rerun preserves registered data',flush=True)

schema='ape_registration_migration_test'
mysql('CREATE DATABASE '+schema)
fixture=(ROOT/'test/registration_fixture.sql').read_text().replace('USE ape_registration_test;','USE '+schema+';')
mysql(fixture, schema)
mysql("ALTER TABLE users DROP INDEX uk_phone; INSERT INTO users(account,phone,nickname,password_hash,password_salt) "
      "VALUES(1000000000,'13800000000','a','h','s'),(1000000001,'13800000000','b','h','s'); "
      "UPDATE account_pool_cursor SET next_start=1000000002",schema)
r=mysql(migration,schema,ok=False)
assert 'Duplicate accounts/phones' in r.stderr, r.stderr
mysql('DELETE FROM users WHERE account=1000000001; UPDATE users SET account=0',schema)
r=mysql(migration,schema,ok=False)
assert 'Invalid or unfinished' in r.stderr,r.stderr
mysql('UPDATE users SET account=1000000000; UPDATE account_pool_cursor SET next_start=1000000000',schema)
r=mysql(migration,schema,ok=False)
assert 'Unsafe/missing cursor' in r.stderr,r.stderr
assert mysql('SELECT next_start FROM account_pool_cursor',schema).stdout.strip()=='1000000000'
mysql('UPDATE account_pool_cursor SET next_start=1000000002',schema)
mysql(migration,schema)
assert mysql("SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() AND "
             "table_name='users' AND column_name='phone' AND non_unique=0",schema).stdout.strip()=='1'
print('PASS migration rejects duplicate phones, incomplete users, and unsafe cursor; adds phone uniqueness',flush=True)

# Validate full fresh-install SQL, exclusively inside the temporary container.
mysql((ROOT/'docker/mysql/init.sql').read_text())
mysql(migration,'ape_auth')
print('PASS fresh initialization including seeded-user cursor',flush=True)

log_path=ROOT/'build/registration-proxy.log'
with log_path.open('w') as log:
    proxy=subprocess.Popen(['python3','-u',str(ROOT/'test/registration_commit_proxy.py')],stdout=log,stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            if proxy.poll() is not None: raise RuntimeError(log_path.read_text())
            if 'READY' in log_path.read_text(): break
            time.sleep(.1)
        else: raise RuntimeError('fault proxy did not start')
        run([str(ROOT/'build/registration_integration'),'--lost-commit'],timeout=60)
        assert 'DROPPED_COMMIT_ACK' in log_path.read_text(), 'fault was not injected'
    finally:
        proxy.terminate()
        proxy.wait(timeout=10)
print('PASS COMMIT fault injection was observed',flush=True)