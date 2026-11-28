import base64
import os
import socket
import ssl
import struct
import subprocess
import sys
import time

sys.path.insert(0, "/tmp/ape-smoke-pyproto")
import apeauth_pb2
import sdkws_pb2


class WS:
    def __init__(self, port=6666):
        raw = socket.create_connection(("127.0.0.1", port), timeout=5)
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
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("handshake closed")
            resp += chunk
        if b"101" not in resp.split(b"\r\n")[0]:
            raise RuntimeError("handshake failed")

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    def send_raw(self, data):
        header = bytearray([0x82])
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        header += mask
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes(header) + payload)

    def recv_raw(self, timeout=5):
        self.sock.settimeout(timeout)
        try:
            hdr = self._recv_exact(2)
        except (socket.timeout, ConnectionError):
            return None
        opcode = hdr[0] & 0x0F
        masked = hdr[1] & 0x80
        length = hdr[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 0x8:
            return None
        return payload

    def send(self, req):
        self.send_raw(req.SerializeToString())

    def recv(self, timeout=5):
        payload = self.recv_raw(timeout)
        if payload is None:
            return None
        resp = sdkws_pb2.SdkWSResp()
        if not resp.ParseFromString(payload):
            raise RuntimeError("bad SdkWSResp")
        return resp


def sql(statement):
    subprocess.run(
        ["docker", "exec", "mysql", "mysql", "-uape_user", "-pape_password", "ape_auth", "-e", statement],
        check=True,
        capture_output=True,
        text=True,
    )


def register(phone, nickname, password):
    ws = WS()
    req = sdkws_pb2.SdkWSReq(type=90, requestId="reg-" + phone)
    reg = apeauth_pb2.RegisterRequest(phone=phone, nickname=nickname, password=password)
    req.data = reg.SerializeToString()
    ws.send(req)
    resp = ws.recv()
    print("register resp", resp)
    ws.sock.close()
    return resp.account


def login(account, password):
    ws = WS()
    req = sdkws_pb2.SdkWSReq(type=91, account=account, deviceID="probe", requestId="login-" + str(account))
    req.data = password.encode()
    ws.send(req)
    resp = ws.recv()
    print("login resp", resp)
    while True:
        extra = ws.recv(timeout=3)
        if extra is None:
            break
        print("post-login", extra.type, extra.errorCode)
        if extra.type == 210:
            break
    return ws, resp.token


def main():
    stamp = str(int(time.time()))[-8:]
    pa = "139" + stamp
    pb = "138" + stamp
    a = register(pa, "probeA", "pw123456")
    b = register(pb, "probeB", "pw123456")
    print("accounts", a, b)
    if not a or not b:
        return
    sql(
        "INSERT IGNORE INTO friend_pairs(low_account,high_account) VALUES ({},{});"
        "INSERT IGNORE INTO friends(owner_account,friend_account,created_time) VALUES ({},{},UNIX_TIMESTAMP()),({},{},UNIX_TIMESTAMP());".format(
            min(a, b), max(a, b), a, b, b, a
        )
    )
    wa, ta = login(a, "pw123456")
    wb, tb = login(b, "pw123456")
    msg = sdkws_pb2.MsgData(
        sendID=a,
        recvID=b,
        clientMsgID="probe-msg-1",
        sessionType=1,
        contentType=101,
        content=b"hello from probe",
        sendTime=time.time(),
    )
    send = sdkws_pb2.SendMessageReq()
    send.msgs.append(msg)
    req = sdkws_pb2.SdkWSReq(type=101, token=ta, account=a, requestId="send-1", data=send.SerializeToString())
    t0 = time.time()
    wa.send(req)
    ra = wa.recv(timeout=10)
    print("send resp", ra)
    if ra and ra.data:
        sr = sdkws_pb2.SendMessageResp()
        sr.ParseFromString(ra.data)
        print("send infos", sr)
    rb = wb.recv(timeout=10)
    while rb and rb.type not in (104, 200):
        print("recv other", rb.type, rb.errorCode)
        rb = wb.recv(timeout=3)
    print("push after ms", (time.time() - t0) * 1000, rb)
    if rb and rb.data:
        md = sdkws_pb2.MsgData()
        md.ParseFromString(rb.data)
        print("pushed msg", md)
        ack = sdkws_pb2.AckReq(userID=b, ackType=1)
        ca = ack.conversations.add()
        ca.conversationID = md.convID
        ca.upToSeq = md.seq
        ack_ws = sdkws_pb2.SdkWSReq(type=106, token=tb, account=b, requestId="ack-1", data=ack.SerializeToString())
        wb.send(ack_ws)


if __name__ == "__main__":
    main()
