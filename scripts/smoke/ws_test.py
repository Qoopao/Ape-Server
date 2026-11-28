"""多网关冒烟测试：注册 / 登录 / 路由表 / 跨实例互踢 / 心跳 CAS / 209 同步摘要。

只用手写 TLS WebSocket 帧 + protoc 生成的 python 绑定，不依赖第三方库。
"""
import base64
import os
import socket
import ssl
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
import sdkws_pb2  # noqa: E402
import apeauth_pb2  # noqa: E402


class WS:
    def __init__(self, port):
        raw = socket.create_connection(('127.0.0.1', port), timeout=5)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.sock = ctx.wrap_socket(raw)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError('handshake closed')
            resp += chunk
        status = resp.split(b'\r\n')[0]
        if b'101' not in status:
            raise RuntimeError(f'handshake failed: {status!r}')

    def _recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError('closed')
            buf += chunk
        return buf

    def send(self, data):
        header = bytearray([0x82])
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack('!H', n)
        else:
            header.append(0x80 | 127)
            header += struct.pack('!Q', n)
        header += mask
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes(header) + payload)

    def recv(self, timeout=5):
        self.sock.settimeout(timeout)
        try:
            hdr = self._recv_exact(2)
        except (socket.timeout, ConnectionError):
            return None
        opcode = hdr[0] & 0x0F
        masked = hdr[1] & 0x80
        length = hdr[1] & 0x7F
        if length == 126:
            length = struct.unpack('!H', self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack('!Q', self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 0x8:
            return None
        return payload

    def recv_proto(self, timeout=5):
        payload = self.recv(timeout)
        if payload is None:
            return None
        resp = sdkws_pb2.SdkWSResp()
        resp.ParseFromString(payload)
        return resp


def redis_get(key):
    out = subprocess.run(['redis-cli', '-a', 'redis123', 'GET', key],
                         capture_output=True, text=True)
    return out.stdout.strip()


def redis_set(key, value):
    subprocess.run(['redis-cli', '-a', 'redis123', 'SET', key, value, 'EX', '300'],
                   capture_output=True, text=True)


def expect(cond, message):
    if not cond:
        raise AssertionError(message)
    print('  ok:', message)


def main():
    account = 0
    # 1) 注册
    ws = WS(6666)
    req = sdkws_pb2.SdkWSReq()
    req.type = 90
    phone = '138' + str(int(time.time()))[-8:]
    req.requestId = 'reg-' + phone
    reg = apeauth_pb2.RegisterRequest()
    reg.phone = phone
    reg.nickname = 'smoke'
    reg.password = 'pw123456'
    req.data = reg.SerializeToString()
    ws.send(req.SerializeToString())
    resp = ws.recv_proto()
    expect(resp is not None and resp.account != 0,
           f'register returned account={None if resp is None else resp.account}')
    account = resp.account
    ws.sock.close()

    # 2) gw-1 登录（密码）：注册成功后服务端会关闭连接，必须新建连接
    ws = WS(6666)
    login_req = sdkws_pb2.SdkWSReq()
    login_req.type = 91
    login_req.account = account
    login_req.deviceID = 'dev-1'
    login_req.requestId = 'login-gw1'
    login_req.data = b'pw123456'
    ws.send(login_req.SerializeToString())
    login = ws.recv_proto()
    if login is None:
        raise AssertionError('gw-1 login got no response')
    expect(login.errorCode == '' and login.token != '',
           f'gw-1 password login succeeded (errorCode={login.errorCode!r}, '
           f'msg={login.errorMsg!r})')
    expect(login.account == account, 'login response carries account')

    # 3) 209 同步摘要（新用户没有会话），随后必须收 210（群同步结束）
    sync = ws.recv_proto(timeout=8)
    expect(sync is not None and sync.type == 209, 'received type=209 sync summary')
    summary = sdkws_pb2.SyncSummary()
    summary.ParseFromString(sync.data)
    expect(sync.errorCode in ('0', 'PARTIAL'),
           f'sync summary errorCode={sync.errorCode}')
    expect(summary.conversationsTotal == 0, 'new account has 0 conversations')
    group_sync = ws.recv_proto(timeout=8)
    expect(group_sync is not None and group_sync.type == 210,
           'received type=210 group sync summary')

    route = redis_get(f'gw:user:{account}')
    expect(route.startswith('gw-1:'), f'route points at gw-1 ({route})')
    first_session = route.split(':', 1)[1]

    # 4) 心跳（pong + 路由 CAS 续期）
    ping = sdkws_pb2.SdkWSReq()
    ping.type = 0
    ping.requestId = 'ping-1'
    ws.send(ping.SerializeToString())
    pong = ws.recv_proto()
    expect(pong is not None and pong.errorCode == '0' and pong.errorMsg == 'pong',
           'heartbeat answered pong')
    expect(redis_get(f'gw:user:{account}') == route, 'heartbeat renewed same route')

    # 5) 同账号在 gw-2 登录 -> gw-1 旧连接被踢，路由切换
    ws2 = WS(6667)
    login2 = sdkws_pb2.SdkWSReq()
    login2.type = 91
    login2.account = account
    login2.deviceID = 'dev-2'
    login2.requestId = 'login-gw2'
    login2.data = b'pw123456'
    ws2.send(login2.SerializeToString())
    login2_resp = ws2.recv_proto()
    expect(login2_resp is not None and login2_resp.errorCode == '',
           'gw-2 login succeeded')
    sync2 = ws2.recv_proto(timeout=8)
    expect(sync2 is not None and sync2.type == 209, 'gw-2 sync summary received')
    group_sync2 = ws2.recv_proto(timeout=8)
    expect(group_sync2 is not None and group_sync2.type == 210,
           'gw-2 group sync summary received')

    kicked = ws.recv_proto(timeout=5)
    expect(kicked is not None and kicked.errorCode == 'KICKED',
           'gw-1 old connection received KICKED')

    route2 = redis_get(f'gw:user:{account}')
    expect(route2.startswith('gw-2:'), f'route switched to gw-2 ({route2})')
    expect(route2.split(':', 1)[1] != first_session, 'new session id differs')

    # 6) CAS 自愈：篡改路由后，gw-2 下一次心跳应自杀
    redis_set(f'gw:user:{account}', 'gw-9:fake')
    ping2 = sdkws_pb2.SdkWSReq()
    ping2.type = 0
    ping2.requestId = 'ping-2'
    ws2.send(ping2.SerializeToString())
    lost = ws2.recv_proto(timeout=5)
    expect(lost is not None and lost.errorCode == 'KICKED',
           'CAS failure self-kicks the stale connection')

    # 7) etcd 注册：每个实例一个 key，值为可路由地址（不是 0.0.0.0）
    out = subprocess.run(
        ['etcdctl', '--endpoints=127.0.0.1:2379', 'get', '--prefix',
         '/services/GatewayPushService', '--write-out=json'],
        capture_output=True, text=True)
    import json
    payload = json.loads(out.stdout)
    kvs = payload.get('kvs', [])
    keys = [base64.b64decode(kv['key']).decode() for kv in kvs]
    values = [base64.b64decode(kv['value']).decode() for kv in kvs]
    expect(len(keys) >= 2, f'etcd has per-instance gateway keys: {keys}')
    expect(all('/services/GatewayPushService/gw-' in k for k in keys),
           'gateway etcd keys carry instance id')
    expect(all('0.0.0.0' not in v for v in values),
           'registered addresses are routable (no 0.0.0.0)')
    expect(all('"methods"' in v or 'PushFrame' in v for v in values),
           'etcd value carries methods JSON')

    print('ALL SMOKE CHECKS PASSED')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as e:
        print('SMOKE FAILED:', e)
        sys.exit(1)
