"""单进程兼容模式（--role=all）回归：注册 / 登录 / 209 / 路由表。"""
import os
import sys
import time

sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sdkws_pb2  # noqa: E402,F401
from ws_test import expect, redis_get  # noqa: E402
import group_test  # noqa: E402
import subprocess  # noqa: E402


def main():
    stamp = str(int(time.time()))[-8:]
    account = group_test.register(6666, '135' + stamp)
    ws = group_test.login(6666, account)
    print('  ok: role=all register+login+209, account=', account)

    route = redis_get(f'gw:user:{account}')
    host = subprocess.run(['hostname'], capture_output=True, text=True).stdout.strip()
    expect(route.startswith(host + ':50055:'),
           f'role=all instance id is hostname:50055 ({route})')
    print('ALL-MODE SMOKE PASSED')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('ALL-MODE SMOKE FAILED:', exc)
        sys.exit(1)
