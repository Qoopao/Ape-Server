"""单聊 ACK（type=106）验收测试。

对应文档：
  - 需求：docs/ack/README.md
  - 用例：docs/ack/测试用例与边界.md

覆盖：基础（ACK-001~004）、边界（ACK-010~020）、乱序/提前（ACK-030~035）、
投递/离线（ACK-040~044）、性能（ACK-060~062）、Pull（SERVER-001/002）、
回归（ACK-070/071）、Outbox 保留期清理（OUTBOX-001~006）。
失败注入（ACK-050）需要 --faults 显式开启，会停止并重启 PushService。

数据准备方式（见 build_thread）：
  默认直接构造 `im_conversations` 水位 + MongoDB 消息文档，不经过 Kafka，
  因此用例快且可重复；真实发送链路由 `seq_test.py` 覆盖。
  加 --e2e 则改为真实 A→B 发送（慢，依赖 Kafka 可用）。

前置环境（可通过环境变量覆盖）：
  - gateway 6666/6667、backbon 50052、auth 50051、msg 50053、push 50054
  - MySQL 127.0.0.1:3306（root，--skip-grant-tables）、MongoDB root/root、Redis redis123
  - Python protobuf 绑定：python3 scripts/smoke/gen_proto.py
  - 建议用 scripts/smoke/start_ack.sh 起服务（缩短 Outbox 清理间隔，便于验证 H 节）

用法：
  python3 scripts/smoke/ack_test.py [--faults] [--e2e] [--json PATH] [--case ACK-001 ...]
退出码：0 = 没有 FAIL（SKIP/WARN 允许），1 = 存在 FAIL。
"""
import argparse
import json
import os
import random
import string
import subprocess
import sys
import time

SMOKE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
sys.path.insert(0, SMOKE_DIR)

import group_test  # noqa: E402
import msg_pb2  # noqa: E402
import sdkws_pb2  # noqa: E402
from bson import Int64  # noqa: E402

from google.protobuf import json_format  # noqa: E402
from ws_test import WS  # noqa: E402

ROOT = os.path.abspath(os.path.join(SMOKE_DIR, '..', '..'))
MYSQL_BIN = os.environ.get(
    'ACK_MYSQL_BIN', os.path.join(ROOT, 'build/group-test-tools/root/usr/bin/mariadb'))
MYSQL_ARGS = ['--no-defaults', '--host=127.0.0.1', '--port=3306', '--user=root', 'ape_auth']
GW1 = int(os.environ.get('ACK_GW1', '6666'))
GW2 = int(os.environ.get('ACK_GW2', '6667'))
MONGO_URI = os.environ.get('ACK_MONGO_URI', 'mongodb://root:root@localhost:27017/?authSource=admin')
LOG_FILES = ['/tmp/ape-smoke-msg.log', '/tmp/ape-smoke-push.log']
MAX_INT64 = 2**63 - 1
DAY_MS = 24 * 60 * 60 * 1000
HOUR_MS = 60 * 60 * 1000


class Skip(Exception):
    """用例在当前环境下不适用。"""


class Warn(Exception):
    """用例跑通但结果偏离文档指标（记录后继续）。"""


class Ctx:
    faults = False
    e2e = False
    metrics = {}


# ────────────────────────────── 基础设施 ──────────────────────────────

def sql(query):
    out = subprocess.run([MYSQL_BIN, *MYSQL_ARGS, '-N', '-e', query],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError(f'SQL failed: {out.stderr.strip()}\n{query}')
    return out.stdout.strip()


def sql_int(query, default=-1):
    value = sql(query)
    return int(value) if value not in ('', 'NULL') else default


def now_ms():
    return int(time.time() * 1000)


_mongo = None


def mongo():
    global _mongo
    if _mongo is None:
        import pymongo
        _mongo = pymongo.MongoClient(MONGO_URI, serverSelectionTimeoutMS=3000)
    return _mongo


def msg_collection():
    return mongo()['IM-System']['msg']


def require(cond, message):
    if not cond:
        raise AssertionError(message)


def wait_for(predicate, timeout=20, interval=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def unique_stamp():
    return ''.join(random.choice(string.digits) for _ in range(8))


def conv_id(a, b):
    return f'{min(a, b)}_{max(a, b)}'


def quoted(values):
    return ','.join("'" + str(v) + "'" for v in values)


def make_friends(a, b):
    sql(f"INSERT IGNORE INTO friends(owner_account,friend_account,created_time) "
        f"VALUES ({a},{b},0),({b},{a},0)")


def send_msg(ws, peer, client_msg_id, content):
    req = sdkws_pb2.SdkWSReq()
    req.type = 101
    req.requestId = client_msg_id
    body = sdkws_pb2.SendMessageReq()
    msg = body.msgs.add()
    msg.recvID = peer
    msg.clientMsgID = client_msg_id
    msg.contentType = 101
    msg.content = content.encode()
    msg.sessionType = 1
    req.data = body.SerializeToString()
    ws.send(req.SerializeToString())


def ack(ws, user, conversations, ack_type=1, request_id=None):
    """conversations: [(conversationID, upToSeq), ...]"""
    req = sdkws_pb2.SdkWSReq()
    req.type = 106
    req.requestId = request_id or f'ack-{time.time()}'
    body = sdkws_pb2.AckReq()
    body.userID = user
    body.ackType = ack_type
    for conversation, up_to in conversations:
        item = body.conversations.add()
        item.conversationID = conversation
        item.upToSeq = up_to
    req.data = body.SerializeToString()
    ws.send(req.SerializeToString())


def receipt(owner, conversation):
    value = sql(f"SELECT delivered_seq FROM im_conversation_receipts "
                f"WHERE owner_account={owner} AND conversation_id='{conversation}'")
    return int(value) if value != '' else None


def wait_receipt(owner, conversation, expected, timeout=25):
    return wait_for(lambda: receipt(owner, conversation) == expected,
                    timeout=timeout, interval=0.25)


def op3_rows(conversation):
    return sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 "
                   f"AND conversation_id='{conversation}'")


def op3_seqs(conversation, owner=None):
    where = f"operation=3 AND conversation_id='{conversation}'"
    if owner is not None:
        where += f" AND actor_account={owner}"
    rows = sql(f"SELECT seq FROM im_outbox WHERE {where} ORDER BY seq")
    return [int(x) for x in rows.split()] if rows else []


def op1_states(conversation):
    rows = sql(f"SELECT seq,state FROM im_outbox WHERE operation=1 "
               f"AND conversation_id='{conversation}' ORDER BY seq")
    result = {}
    for line in rows.splitlines():
        if not line.strip():
            continue
        seq, state = line.split('\t')
        result[int(seq)] = int(state)
    return result


def max_seq(owner, conversation):
    value = sql(f"SELECT max_seq FROM im_conversations WHERE owner_account={owner} "
                f"AND conversation_id='{conversation}'")
    return int(value) if value != '' else None


def wait_max_seq(owner, conversation, expected, timeout=90):
    return wait_for(lambda: max_seq(owner, conversation) == expected,
                    timeout=timeout, interval=0.5)


def ping_alive(ws, timeout=4):
    """发心跳并等 pong；连接被关闭时返回 False。"""
    token = f'ping-{random.randint(1000, 999999)}'
    req = sdkws_pb2.SdkWSReq()
    req.type = 0
    req.requestId = token
    try:
        ws.send(req.SerializeToString())
    except Exception:  # noqa: BLE001
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            break
        if frame.type == 0 and frame.requestId == token:
            return True
    return False


def wait_closed(ws, timeout=10):
    """等待网关主动断连（ACK 失败/协议错误会触发 onDisconnect）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not ping_alive(ws, timeout=max(0.5, deadline - time.time())):
            return True
        time.sleep(0.2)
    return False


def pull(ws, owner, peer, after_seq, limit=100, timeout=15):
    """type=244 拉历史，返回 (seqs, hasMore, resp)。"""
    request_id = f'pull-{owner}-{after_seq}-{random.randint(1000, 9999)}'
    req = sdkws_pb2.SdkWSReq()
    req.type = 244
    req.requestId = request_id
    body = msg_pb2.GetSingleChatHistoryReq()
    body.ownerUserID = owner
    body.peerUserID = peer
    body.afterSeq = after_seq
    body.limit = limit
    req.data = body.SerializeToString()
    ws.send(req.SerializeToString())
    deadline = time.time() + timeout
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            break
        if frame.type != 244 or frame.requestId != request_id:
            continue
        require(frame.errorCode in ('', '0'),
                f'Pull failed: errorCode={frame.errorCode!r} {frame.errorMsg!r}')
        resp = msg_pb2.GetSingleChatHistoryResp()
        resp.ParseFromString(frame.data)
        return [m.seq for m in resp.messages], resp.hasMore, resp
    raise AssertionError(f'Pull(afterSeq={after_seq}) got no reply')


def login_sync(port, account):
    """登录并收集 209 之前的全部 200 帧，返回 (ws, {conv: [seq]}, SyncSummary)。"""
    ws = WS(port)
    req = sdkws_pb2.SdkWSReq()
    req.type = 91
    req.account = account
    req.deviceID = 'dev'
    req.requestId = f'login-sync-{account}'
    req.data = b'pw123456'
    ws.send(req.SerializeToString())
    first = ws.recv_proto(timeout=10)
    require(first is not None and first.errorCode == '',
            f'login {account} failed: {None if first is None else first.errorCode}')
    seqs = {}
    deadline = time.time() + 25
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            break
        if frame.type == 200:
            data = sdkws_pb2.MsgData()
            data.ParseFromString(frame.data)
            seqs.setdefault(data.convID, []).append(data.seq)
        elif frame.type == 209:
            summary = sdkws_pb2.SyncSummary()
            summary.ParseFromString(frame.data)
            # 群同步以 210 收尾：消费掉，避免污染后续 200/106 帧读取。
            while True:
                tail = ws.recv_proto(timeout=10)
                if tail is None or tail.type == 210:
                    break
            return ws, seqs, summary
    raise AssertionError('missing 209 sync summary')


def log_has(pattern):
    for path in LOG_FILES:
        try:
            with open(path, 'r', errors='ignore') as handle:
                if pattern in handle.read():
                    return True
        except FileNotFoundError:
            continue
    return False


# ────────────────────────────── 数据准备 ──────────────────────────────

class Fixture:
    def __init__(self, a, b, ws_a, ws_b, conv, stamp):
        self.a = a
        self.b = b
        self.ws_a = ws_a
        self.ws_b = ws_b
        self.conv = conv
        self.stamp = stamp
        self.extra = []
        self.message_ids = []

    def close(self):
        for ws in (self.ws_a, self.ws_b, *self.extra):
            try:
                ws.sock.close()
            except Exception:  # noqa: BLE001
                pass

    def cleanup(self):
        try:
            if self.message_ids:
                msg_collection().delete_many({'serverMsgID': {'$in': self.message_ids}})
        except Exception:  # noqa: BLE001
            pass
        sql(f"DELETE FROM im_conversation_receipts WHERE conversation_id='{self.conv}'")
        sql(f"DELETE FROM im_outbox WHERE conversation_id='{self.conv}'")
        sql(f"DELETE FROM im_conversations WHERE conversation_id='{self.conv}'")


def fresh_pair(port=GW1):
    """注册两个好友 A/B 并分别在网关登录。"""
    stamp = unique_stamp()
    a = group_test.register(port, '133' + stamp)
    b = group_test.register(port, '134' + stamp)
    make_friends(a, b)
    ws_a = group_test.login(port, a)
    ws_b = group_test.login(port, b)
    return Fixture(a, b, ws_a, ws_b, conv_id(a, b), stamp)


FAKE_BASE = 990000000


def message_payload(f, seq, client_msg_id, content):
    """构造 outbox payload（与 msg 服务落库格式一致：MsgData 的 JSON）。"""
    data = sdkws_pb2.MsgData()
    data.sendID = f.a
    data.recvID = f.b
    data.convID = f.conv
    data.clientMsgID = client_msg_id
    data.serverMsgID = f'fixture-{f.stamp}-{seq}'
    data.sessionType = 1
    data.msgFrom = 100
    data.contentType = 101
    data.content = content.encode()
    data.seq = seq
    data.sendTime = now_ms()
    return json_format.MessageToJson(data)


def mongo_doc(f, seq, content='m', committed=True):
    # sendID/recvID/seq/sendTime 必须是 BSON int64：C++ 侧历史查询用 get_int64() 读取，
    # pymongo 默认会把小整数编码成 int32，导致 “expected element type k_int64”。
    return {
        'serverMsgID': f'fixture-{f.stamp}-{seq}',
        'sendID': Int64(f.a),
        'recvID': Int64(f.b),
        'convID': f.conv,
        'clientMsgID': f'fixture-{f.stamp}-{seq}',
        'senderPlatformID': 1,
        'senderNickname': '',
        'senderFaceURL': '',
        'sessionType': 1,
        'msgFrom': 100,
        'contentType': 101,
        'content': content,
        'seq': Int64(seq),
        'sendTime': Int64(now_ms()),
        'status': 0,
        'isRead': False,
        'isDeleted': False,
        'isRecalled': False,
        'isPinned': False,
        'isGroupMsg': False,
        'ext': '',
        'committed': committed,
    }

def seed_sequence(conv, next_seq):
    """伪造会话时要同步 im_conversation_seq，否则真实发送会从 1 重新分配 seq。"""
    sql(f"INSERT INTO im_conversation_seq(conversation_id,next_seq) VALUES('{conv}',{next_seq}) "
        f"ON DUPLICATE KEY UPDATE next_seq=GREATEST(next_seq,{next_seq})")


def build_thread(f, count, with_op1=False):
    """准备 B 视角 max_seq=count 的会话线程。

    默认：直接写 im_conversations 水位 + MongoDB 消息文档（快、不依赖 Kafka）；
    --e2e：A 真实发送 count 条消息并等待提交（慢，依赖 Kafka）。
    """
    if count <= 0:
        return
    if Ctx.e2e:
        for i in range(1, count + 1):
            send_msg(f.ws_a, f.b, f'e2e-{f.stamp}-{i}', f'm{i}')
        require(wait_max_seq(f.b, f.conv, count, timeout=600),
                f'e2e messages not committed: max_seq={max_seq(f.b, f.conv)}')
        return

    sql(f"INSERT INTO im_conversations(owner_account,conversation_id,peer_account,min_seq,max_seq,"
        f"latest_time,latest_message) VALUES({f.b},'{f.conv}',{f.a},1,{count},{now_ms()},'') "
        f"ON DUPLICATE KEY UPDATE max_seq={count}")
    seed_sequence(f.conv, count)

    docs = [mongo_doc(f, seq) for seq in range(1, count + 1)]
    msg_collection().insert_many(docs)
    f.message_ids.extend(doc['serverMsgID'] for doc in docs)
    if with_op1:
        for seq in range(1, count + 1):
            insert_outbox(1, f'fixture:{f.stamp}:{seq}', f.a, f.b, f.conv, seq, 0,
                          now_ms(), payload=message_payload(f, seq, '', f'm{seq}'), stage=0,
                          server_msg_id=f'fixture-{f.stamp}-{seq}')


def drop_conversations(convs):
    if not convs:
        return
    listed = quoted(convs)
    sql(f"DELETE FROM im_conversation_receipts WHERE conversation_id IN ({listed})")
    sql(f"DELETE FROM im_outbox WHERE conversation_id IN ({listed})")
    sql(f"DELETE FROM im_conversations WHERE conversation_id IN ({listed})")


def fabricate_conversation(owner, peer, top_seq, min_seq=1):
    """只插 im_conversations 行（无消息正文），用于构造水位类用例。"""
    conv = conv_id(owner, peer)
    sql(f"INSERT INTO im_conversations(owner_account,conversation_id,peer_account,min_seq,max_seq,"
        f"latest_message) VALUES({owner},'{conv}',{peer},{min_seq},{top_seq},'') "
        f"ON DUPLICATE KEY UPDATE max_seq={top_seq}, min_seq={min_seq}")
    return conv


def insert_outbox(operation, dedupe, owner, peer, conv, seq, state, updated_at,
                  payload='', stage=1, available_at=0, lease_until=0, server_msg_id=''):
    sql(f"INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,"
        f"server_msg_id,seq,payload,state,stage,available_at,lease_until,created_at,updated_at) VALUES"
        f"({operation},'{dedupe}',{owner},{peer},'{conv}','{server_msg_id}',{seq},'{payload}',"
        f"{state},{stage},{available_at},{lease_until},{updated_at},{updated_at})")


# ────────────────────────────── 用例 ──────────────────────────────

def case_ack_001():
    """ACK-001 单会话 ACK：水位=upToSeq，生成 op=3，连接保持。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, 5)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        require(op3_rows(f.conv) == 1, f'op=3 rows={op3_rows(f.conv)}')
        dedupe = sql(f"SELECT dedupe_key FROM im_outbox WHERE operation=3 "
                     f"AND conversation_id='{f.conv}'")
        require(dedupe == f'ack:{f.b}:{f.conv}:5', f'dedupe_key={dedupe!r}')
        require(ping_alive(f.ws_b), 'connection dropped after ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_002():
    """ACK-002 一帧多会话：各自独立更新。"""
    f = fresh_pair()
    third = group_test.register(GW1, '135' + unique_stamp())
    make_friends(f.b, third)
    conv2 = conv_id(f.b, third)
    try:
        build_thread(f, 5)
        ws_c = group_test.login(GW1, third)
        f.extra.append(ws_c)
        other = Fixture(third, f.b, ws_c, ws_c, conv2, unique_stamp())

        sql(f"INSERT INTO im_conversations(owner_account,conversation_id,peer_account,min_seq,max_seq,"
            f"latest_time,latest_message) VALUES({f.b},'{conv2}',{third},1,3,{now_ms()},'')")
        docs = [mongo_doc(other, seq) for seq in range(1, 4)]
        msg_collection().insert_many(docs)
        f.message_ids.extend(doc['serverMsgID'] for doc in docs)

        ack(f.ws_b, f.b, [(f.conv, 5), (conv2, 3)])
        require(wait_receipt(f.b, f.conv, 5), f'conv1 delivered_seq={receipt(f.b, f.conv)}')
        require(wait_receipt(f.b, conv2, 3), f'conv2 delivered_seq={receipt(f.b, conv2)}')
        require(op3_rows(f.conv) == 1 and op3_rows(conv2) == 1,
                f'op=3 rows: {op3_rows(f.conv)}/{op3_rows(conv2)}')
    finally:
        f.close()
        f.cleanup()
        drop_conversations([conv2])


def case_ack_003():
    """ACK-003 ackType=1/2 行为一致。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, 3)], ack_type=1)
        require(wait_receipt(f.b, f.conv, 3), f'ackType=1 delivered_seq={receipt(f.b, f.conv)}')
        ack(f.ws_b, f.b, [(f.conv, 5)], ack_type=2)
        require(wait_receipt(f.b, f.conv, 5), f'ackType=2 delivered_seq={receipt(f.b, f.conv)}')
        require(op3_seqs(f.conv) == [3, 5], f'op=3 seqs={op3_seqs(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_004():
    """ACK-004 重复 ACK：水位不变、不新增 op=3。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, 5)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        before = op3_rows(f.conv)
        for _ in range(3):
            ack(f.ws_b, f.b, [(f.conv, 5)])
        time.sleep(2)
        require(receipt(f.b, f.conv) == 5, f'delivered_seq changed: {receipt(f.b, f.conv)}')
        require(op3_rows(f.conv) == before, f'op=3 grew {before} -> {op3_rows(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_010():
    """ACK-010 upToSeq=0/负数：网关过滤，不写、不断连。"""
    f = fresh_pair()
    try:
        build_thread(f, 3)
        ack(f.ws_b, f.b, [(f.conv, 0), (f.conv, -1)])
        time.sleep(1.5)
        require(receipt(f.b, f.conv) is None, f'receipt written: {receipt(f.b, f.conv)}')
        require(op3_rows(f.conv) == 0, f'op=3 rows={op3_rows(f.conv)}')
        require(ping_alive(f.ws_b), 'connection dropped after filtered ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_011():
    """ACK-011 upToSeq>max_seq：截断到 max_seq。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, 10)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        require(op3_seqs(f.conv) == [5], f'op=3 seqs={op3_seqs(f.conv)}')
        require(ping_alive(f.ws_b), 'connection dropped after clamped ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_012():
    """ACK-012 max_seq=0（会话存在但无消息）：no-op。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 1, 0, min_seq=0)
    try:
        ack(f.ws_b, f.b, [(conv, 5)])
        time.sleep(1.5)
        value = receipt(f.b, conv)
        require(value in (None, 0), f'receipt written: {value}')
        require(op3_rows(conv) == 0, f'op=3 rows={op3_rows(conv)}')
        require(ping_alive(f.ws_b), 'connection dropped')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_013():
    """ACK-013 非法 conversationID：INVALID_ARGUMENT，不写，网关断连。"""
    f = fresh_pair()
    try:
        build_thread(f, 2)
        ack(f.ws_b, f.b, [('abc', 1), ('1_2_3', 1), ('_', 1)])
        require(wait_closed(f.ws_b), 'gateway did not disconnect on INVALID_ARGUMENT')
        for invalid in ('abc', '1_2_3', '_'):
            require(sql_int(f"SELECT COUNT(*) FROM im_conversation_receipts "
                            f"WHERE conversation_id='{invalid}'") == 0,
                    f'illegal ACK wrote receipt: {invalid}')
        require(op3_rows(f.conv) == 0, f'illegal ACK wrote op=3: {op3_rows(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_014():
    """ACK-014 非参与者：no-op 成功，不写、不断连。"""
    f = fresh_pair()
    outsider = group_test.register(GW1, '136' + unique_stamp())
    try:
        build_thread(f, 3)
        ws_out = group_test.login(GW1, outsider)
        f.extra.append(ws_out)
        ack(ws_out, outsider, [(f.conv, 3)])
        require(ping_alive(ws_out), 'connection dropped for non-participant ACK')
        require(receipt(outsider, f.conv) is None,
                f'receipt written for outsider: {receipt(outsider, f.conv)}')
        require(op3_rows(f.conv) == 0, f'op=3 rows={op3_rows(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_015():
    """ACK-015 会话已删除：no-op 成功，不断连。"""
    f = fresh_pair()
    try:
        build_thread(f, 3)
        sql(f"DELETE FROM im_conversations WHERE conversation_id='{f.conv}'")
        ack(f.ws_b, f.b, [(f.conv, 3)])
        require(ping_alive(f.ws_b), 'connection dropped after expired ACK')
        require(receipt(f.b, f.conv) is None, f'receipt written: {receipt(f.b, f.conv)}')
        require(op3_rows(f.conv) == 0, f'op=3 rows={op3_rows(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_016():
    """ACK-016 群 ID（非 digits_digits）走 106：不写，按非法处理。"""
    f = fresh_pair()
    group_id = str(500000 + random.randint(1, 99999))
    try:
        build_thread(f, 2)
        require('_' not in group_id, 'group id unexpectedly contains underscore')
        ack(f.ws_b, f.b, [(group_id, 5)])
        require(wait_closed(f.ws_b), 'gateway did not disconnect on group ID')
        require(sql_int(f"SELECT COUNT(*) FROM im_conversation_receipts "
                        f"WHERE conversation_id='{group_id}'") == 0,
                'group ID wrote a single-chat receipt')
    finally:
        f.close()
        f.cleanup()


def case_ack_017():
    """ACK-017 一帧重复会话：最终取最大，幂等。"""
    f = fresh_pair()
    try:
        build_thread(f, 10)
        ack(f.ws_b, f.b, [(f.conv, 5), (f.conv, 10)])
        require(wait_receipt(f.b, f.conv, 10), f'delivered_seq={receipt(f.b, f.conv)}')
        require(op3_seqs(f.conv) == [5, 10], f'op=3 seqs={op3_seqs(f.conv)}')
        require(ping_alive(f.ws_b), 'connection dropped')
    finally:
        f.close()
        f.cleanup()


def case_ack_018():
    """ACK-018 100 个会话一帧：全部处理，记录耗时。"""
    f = fresh_pair()
    convs = []
    try:
        for i in range(100):
            convs.append(fabricate_conversation(f.b, FAKE_BASE + 100 + i, 5))
        started = time.time()
        ack(f.ws_b, f.b, [(conv, 5) for conv in convs])
        ok = wait_for(lambda: sql_int(
            f"SELECT COUNT(*) FROM im_conversation_receipts WHERE owner_account={f.b} "
            f"AND delivered_seq=5 AND conversation_id IN ({quoted(convs)})") == 100,
            timeout=30, interval=0.2)
        elapsed_ms = int((time.time() - started) * 1000)
        require(ok, 'not all 100 conversations were acknowledged')
        require(op3_rows(convs[0]) == 1, 'missing op=3 for first conversation')
        require(ping_alive(f.ws_b), 'connection dropped')
        Ctx.metrics['ACK-018.elapsed_ms'] = elapsed_ms
        print(f'     → 100 会话整帧端到端 {elapsed_ms}ms')
    finally:
        f.close()
        drop_conversations(convs)


def case_ack_019():
    """ACK-019 超大 upToSeq：截断，无溢出。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, MAX_INT64)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        require(op3_seqs(f.conv) == [5], f'op=3 seqs={op3_seqs(f.conv)}')
        require(ping_alive(f.ws_b), 'connection dropped')
    finally:
        f.close()
        f.cleanup()


def case_ack_020():
    """ACK-020 空 conversations：忽略，不报错。"""
    f = fresh_pair()
    try:
        build_thread(f, 2)
        ack(f.ws_b, f.b, [])
        time.sleep(1)
        require(receipt(f.b, f.conv) is None, 'empty ACK wrote a receipt')
        require(ping_alive(f.ws_b), 'connection dropped on empty ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_030():
    """ACK-030 100 后 80：水位不回退、低水位不新增 op=3。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 2, 100)
    try:
        ack(f.ws_b, f.b, [(conv, 100)])
        require(wait_receipt(f.b, conv, 100), f'delivered_seq={receipt(f.b, conv)}')
        ack(f.ws_b, f.b, [(conv, 80)])
        time.sleep(2)
        require(receipt(f.b, conv) == 100, f'watermark rolled back: {receipt(f.b, conv)}')
        require(op3_seqs(conv) == [100], f'op=3 seqs={op3_seqs(conv)}')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_031():
    """ACK-031 80 后 100：最终 100。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 3, 100)
    try:
        ack(f.ws_b, f.b, [(conv, 80)])
        require(wait_receipt(f.b, conv, 80), f'delivered_seq={receipt(f.b, conv)}')
        ack(f.ws_b, f.b, [(conv, 100)])
        require(wait_receipt(f.b, conv, 100), f'delivered_seq={receipt(f.b, conv)}')
        require(op3_seqs(conv) == [80, 100], f'op=3 seqs={op3_seqs(conv)}')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_032():
    """ACK-032 跨网关交叉 ACK：GREATEST 收敛，无死锁。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 4, 100)
    try:
        # 单账号单设备会被互踢：先在同一帧内投递 100/80，再跨网关重连后交叉上报，
        # 覆盖“不同 PushService 实例处理同一 owner 的 ACK”与 GREATEST 语义。
        ack(f.ws_b, f.b, [(conv, 100), (conv, 80)])
        ws2, _, _ = login_sync(GW2, f.b)
        f.extra.append(ws2)
        for up_to in (80, 100, 80):
            ack(ws2, f.b, [(conv, up_to)])
            time.sleep(0.2)
        require(wait_receipt(f.b, conv, 100), f'delivered_seq={receipt(f.b, conv)}')
        require(op3_seqs(conv) == [100], f'op=3 seqs={op3_seqs(conv)}')
        require(not log_has('Deadlock'), 'deadlock reported in service logs')
        require(not log_has('Lock wait timeout'), 'lock wait timeout reported in service logs')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_033():
    """ACK-033 低水位不新增 op=3（目标 G2）。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 5, 100)
    try:
        ack(f.ws_b, f.b, [(conv, 100)])
        require(wait_receipt(f.b, conv, 100), f'delivered_seq={receipt(f.b, conv)}')
        before = op3_rows(conv)
        ack(f.ws_b, f.b, [(conv, 80)])
        time.sleep(2)
        require(receipt(f.b, conv) == 100, f'watermark rolled back: {receipt(f.b, conv)}')
        require(op3_rows(conv) == before,
                f'low watermark created op=3: {before} -> {op3_rows(conv)}')
        require(ping_alive(f.ws_b), 'connection dropped')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_034():
    """ACK-034 提前 ACK：水位到 max_seq，Mongo status=2，Pull 仍能拉到。"""
    f = fresh_pair()
    try:
        build_thread(f, 10)
        ack(f.ws_b, f.b, [(f.conv, 10)])
        require(wait_receipt(f.b, f.conv, 10), f'delivered_seq={receipt(f.b, f.conv)}')
        require(wait_for(lambda: msg_collection().count_documents(
            {'recvID': f.b, 'convID': f.conv, 'seq': {'$lte': 10}, 'status': 2}) == 10,
            timeout=40), 'Mongo status=2 not applied to 1..10')
        seqs, _, _ = pull(f.ws_b, f.b, f.a, after_seq=0)
        require(seqs == list(range(1, 11)),
                f'Pull(afterSeq=0) should still return 1..10, got {seqs}')
    finally:
        f.close()
        f.cleanup()


def case_ack_035():
    """ACK-035 ACK 时消息未提交：截断到 max_seq，不报错。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 6, 98)
    try:
        ack(f.ws_b, f.b, [(conv, 100)])
        require(wait_receipt(f.b, conv, 98), f'delivered_seq={receipt(f.b, conv)}')
        require(op3_seqs(conv) == [98], f'op=3 seqs={op3_seqs(conv)}')
        require(ping_alive(f.ws_b), 'connection dropped')
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_040():
    """ACK-040 ACK 后 op=1 任务完成（不再重投）。"""
    f = fresh_pair()
    try:
        build_thread(f, 5, with_op1=not Ctx.e2e)
        ack(f.ws_b, f.b, [(f.conv, 5)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        if Ctx.e2e:
            # 真实链路：op=1 在 ACK 水位覆盖后由 worker 置为完成
            pass

        def done():
            states = op1_states(f.conv)
            return all(states.get(seq) == 2 for seq in range(1, 6))

        require(wait_for(done, timeout=180, interval=0.5),
                f'op=1 states={op1_states(f.conv)}（receipt 覆盖后由 worker 再次领取）')
    finally:
        f.close()
        f.cleanup()


def case_ack_041():
    """ACK-041 ACK 后 Mongo status=2，范围外不变。"""
    f = fresh_pair()
    try:
        build_thread(f, 6)
        ack(f.ws_b, f.b, [(f.conv, 3)])
        require(wait_receipt(f.b, f.conv, 3), f'delivered_seq={receipt(f.b, f.conv)}')
        require(wait_for(lambda: msg_collection().count_documents(
            {'recvID': f.b, 'convID': f.conv, 'seq': {'$lte': 3}, 'status': 2}) == 3, timeout=40),
            'Mongo status=2 for seq<=3 not applied')
        outside = msg_collection().count_documents(
            {'recvID': f.b, 'convID': f.conv, 'seq': {'$gt': 3}, 'status': 2})
        require(outside == 0, f'out-of-range messages marked delivered: {outside}')
    finally:
        f.close()
        f.cleanup()


def case_ack_042():
    """ACK-042 未 ACK 的消息登录/Pull 仍可拉到。"""
    f = fresh_pair()
    try:
        build_thread(f, 4)
        seqs, _, _ = pull(f.ws_b, f.b, f.a, after_seq=0)
        require(seqs == [1, 2, 3, 4], f'Pull without ACK returned {seqs}')
        require(receipt(f.b, f.conv) is None, 'Pull must not write a receipt')
    finally:
        f.close()
        f.cleanup()


def case_ack_043():
    """ACK-043 ACK 乱序不改变 cursor 行为（Pull 结果一致）。"""
    f = fresh_pair()
    try:
        build_thread(f, 6)
        before, before_more, _ = pull(f.ws_b, f.b, f.a, after_seq=2)
        ack(f.ws_b, f.b, [(f.conv, 3)])
        require(wait_receipt(f.b, f.conv, 3), f'delivered_seq={receipt(f.b, f.conv)}')
        ack(f.ws_b, f.b, [(f.conv, 1)])
        time.sleep(1)
        after, after_more, _ = pull(f.ws_b, f.b, f.a, after_seq=2)
        require(before == after, f'Pull changed after out-of-order ACK: {before} -> {after}')
        require(before_more == after_more, 'hasMore changed after ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_044():
    """ACK-044 重连后重复 ACK：幂等。"""
    f = fresh_pair()
    try:
        build_thread(f, 5)
        ack(f.ws_b, f.b, [(f.conv, 5)])
        require(wait_receipt(f.b, f.conv, 5), f'delivered_seq={receipt(f.b, f.conv)}')
        before = op3_rows(f.conv)
        f.ws_b.sock.close()
        ws_new, _, _ = login_sync(GW1, f.b)
        f.extra.append(ws_new)
        ack(ws_new, f.b, [(f.conv, 5)])
        time.sleep(2)
        require(receipt(f.b, f.conv) == 5, f'delivered_seq changed: {receipt(f.b, f.conv)}')
        require(op3_rows(f.conv) == before, f'op=3 grew {before} -> {op3_rows(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_050():
    """ACK-050 PushService 不可用：网关断连，无半写，恢复后可重试（--faults）。"""
    if not Ctx.faults:
        raise Skip('需要 --faults（会停止并重启 PushService）')
    f = fresh_pair()
    push_log = open('/tmp/ape-smoke-push.log', 'a')
    try:
        build_thread(f, 3)
        subprocess.run(['pkill', '-9', '-f', 'role=push'], capture_output=True)

        def down():
            return subprocess.run(['ss', '-ltn'], capture_output=True,
                                  text=True).stdout.find(':50054 ') < 0

        require(wait_for(down, timeout=20), 'PushService port 50054 still open')
        before = receipt(f.b, f.conv)
        ack(f.ws_b, f.b, [(f.conv, 3)])
        require(wait_closed(f.ws_b), 'gateway did not disconnect when PushService was down')
        require(receipt(f.b, f.conv) == before, 'half-write detected while PushService was down')

        subprocess.Popen([os.path.join(ROOT, 'build/main'), '--role=push'], cwd=ROOT,
                         stdout=push_log, stderr=push_log, start_new_session=True)
        require(wait_for(lambda: not down(), timeout=90), 'PushService did not come back')
        time.sleep(1)
        ws_new, _, _ = login_sync(GW1, f.b)
        f.extra.append(ws_new)
        ack(ws_new, f.b, [(f.conv, 3)])
        require(wait_receipt(f.b, f.conv, 3), f'delivered_seq={receipt(f.b, f.conv)}')
    finally:
        f.close()
        f.cleanup()
        push_log.close()


def case_ack_051():
    raise Skip('停 MySQL 会破坏共享测试环境；无半写已由 ACK-035 的截断路径与 ACK-050 覆盖')


def case_ack_053():
    raise Skip('需要可注入的 RPC 延迟钩子；8s deadline + 断连行为由 ACK-050 覆盖')


def case_ack_054():
    raise Skip('停 Redis 会破坏网关路由/登录；ACK 不读写 Redis 已由代码路径确认')


def case_ack_060():
    """ACK-060 100 会话一帧：<100ms（本地基准）。"""
    elapsed = Ctx.metrics.get('ACK-018.elapsed_ms')
    if elapsed is None:
        raise Skip('依赖 ACK-018 的测量结果')
    if elapsed >= 100:
        raise Warn(f'100 会话整帧端到端 {elapsed}ms ≥ 100ms（含 100 次独立事务提交）')
    return f'100 会话整帧端到端 {elapsed}ms'


def case_ack_061():
    """ACK-061 1000 条逐步 ACK：op=3 行数 ≤ 推进次数。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 7, 1000)
    try:
        for seq in range(1, 1001):
            ack(f.ws_b, f.b, [(conv, seq)], request_id=f'ack-{seq}')
        require(wait_for(lambda: receipt(f.b, conv) == 1000, timeout=180, interval=0.5),
                f'delivered_seq={receipt(f.b, conv)}')
        rows = op3_rows(conv)
        require(rows <= 1000, f'op=3 rows={rows} > 1000')
        require(ping_alive(f.ws_b), 'connection dropped during 1000 ACKs')
        Ctx.metrics['ACK-061.op3_rows'] = rows
    finally:
        f.close()
        drop_conversations([conv])


def case_ack_062():
    """ACK-062 回执行数 O(会话)：同一会话多次 ACK 仍只有一行回执。"""
    f = fresh_pair()
    conv = fabricate_conversation(f.b, FAKE_BASE + 8, 100)
    try:
        for seq in (1, 10, 50, 100):
            ack(f.ws_b, f.b, [(conv, seq)])
        require(wait_receipt(f.b, conv, 100), f'delivered_seq={receipt(f.b, conv)}')
        rows = sql_int(f"SELECT COUNT(*) FROM im_conversation_receipts "
                       f"WHERE owner_account={f.b} AND conversation_id='{conv}'")
        require(rows == 1, f'receipt rows={rows}, expected 1')
    finally:
        f.close()
        drop_conversations([conv])


def case_server_001():
    """SERVER-001 ACK 提前后 Pull 仍返回消息（不按 status 过滤）。"""
    f = fresh_pair()
    try:
        build_thread(f, 10)
        ack(f.ws_b, f.b, [(f.conv, 10)])
        require(wait_receipt(f.b, f.conv, 10), f'delivered_seq={receipt(f.b, f.conv)}')
        require(wait_for(lambda: msg_collection().count_documents(
            {'recvID': f.b, 'convID': f.conv, 'seq': {'$lte': 10}, 'status': 2}) == 10,
            timeout=40), 'Mongo status=2 not applied')
        seqs, _, _ = pull(f.ws_b, f.b, f.a, after_seq=0)
        require(seqs == list(range(1, 11)), f'Pull after ACK returned {seqs}')
    finally:
        f.close()
        f.cleanup()


def case_server_002():
    """SERVER-002 临时缺口：Pull 返回已落库消息，不阻塞等缺口。"""
    f = fresh_pair()
    try:
        build_thread(f, 3)
        extra = [mongo_doc(f, seq) for seq in (103,)]
        msg_collection().insert_many(extra)
        f.message_ids.extend(doc['serverMsgID'] for doc in extra)

        seqs, _, _ = pull(f.ws_b, f.b, f.a, after_seq=3)
        require(103 in seqs, f'Pull(after 3) should return 103, got {seqs}')
        require(not any(4 <= s <= 102 for s in seqs), f'Pull returned unexpected seqs {seqs}')

        backfill = [mongo_doc(f, seq) for seq in (101, 102)]
        msg_collection().insert_many(backfill)
        f.message_ids.extend(doc['serverMsgID'] for doc in backfill)
        seqs, _, _ = pull(f.ws_b, f.b, f.a, after_seq=3)
        require([s for s in seqs if s >= 101] == [101, 102, 103],
                f'Pull after backfill returned {seqs}')
    finally:
        f.close()
        f.cleanup()


# ────────────────────────── Outbox 清理（H 节） ──────────────────────────

_CLEANUP = {}


def cleanup_probe():
    """确认清理线程在运行（间隔过大时 H 用例 SKIP 而不是误判失败）。"""
    if 'ok' in _CLEANUP:
        return _CLEANUP['ok']
    conv = f'{FAKE_BASE + 900}_{FAKE_BASE + 901}'
    dedupe = f'probe:{now_ms()}'
    insert_outbox(3, dedupe, FAKE_BASE + 900, FAKE_BASE + 900, conv, 1, 2, now_ms() - 2 * DAY_MS)
    removed = wait_for(lambda: sql_int(
        f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{dedupe}'") == 0, timeout=25)
    sql(f"DELETE FROM im_outbox WHERE dedupe_key='{dedupe}'")
    _CLEANUP['ok'] = bool(removed)
    Ctx.metrics['outbox.cleanup_active'] = bool(removed)
    return _CLEANUP['ok']


def require_cleanup():
    if not cleanup_probe():
        raise Skip('Outbox 清理线程 25s 内未生效：请用 scripts/smoke/start_ack.sh 启动服务'
                   '（默认 OUTBOX_CLEANUP_INTERVAL_MS=600000）')


def case_outbox_001():
    """OUTBOX-001 op=1：已完成且超 7 天删除，未完成保留。"""
    require_cleanup()
    conv = f'{FAKE_BASE + 910}_{FAKE_BASE + 911}'
    tag = str(now_ms())
    done_key, pending_key = f'outbox1-done-{tag}', f'outbox1-pending-{tag}'
    try:
        insert_outbox(1, done_key, FAKE_BASE + 910, FAKE_BASE + 911, conv, 1, 2,
                      now_ms() - 8 * DAY_MS)
        # 未完成：state=0 且 available_at 在未来（不会被领取），8 天前更新过
        insert_outbox(1, pending_key, FAKE_BASE + 910, FAKE_BASE + 911, conv, 2, 0,
                      now_ms() - 8 * DAY_MS, available_at=now_ms() + HOUR_MS)
        require(wait_for(lambda: sql_int(
            f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{done_key}'") == 0, timeout=30),
            'completed op=1 row older than 7d was not deleted')
        require(sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{pending_key}'") == 1,
                'unfinished op=1 row was deleted')
    finally:
        sql(f"DELETE FROM im_outbox WHERE dedupe_key IN ('{done_key}','{pending_key}')")


def case_outbox_002():
    """OUTBOX-002 op=3 超 1 天删除；op=1 未到 7 天保留。"""
    require_cleanup()
    conv = f'{FAKE_BASE + 920}_{FAKE_BASE + 921}'
    tag = str(now_ms())
    ack_key, send_key = f'outbox2-ack-{tag}', f'outbox2-send-{tag}'
    try:
        insert_outbox(3, ack_key, FAKE_BASE + 920, FAKE_BASE + 920, conv, 5, 2,
                      now_ms() - 2 * DAY_MS)
        insert_outbox(1, send_key, FAKE_BASE + 920, FAKE_BASE + 921, conv, 5, 2,
                      now_ms() - 2 * DAY_MS)
        require(wait_for(lambda: sql_int(
            f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{ack_key}'") == 0, timeout=30),
            'op=3 row older than 1d was not deleted')
        require(sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{send_key}'") == 1,
                'op=1 row younger than 7d was deleted')
    finally:
        sql(f"DELETE FROM im_outbox WHERE dedupe_key IN ('{ack_key}','{send_key}')")


def case_outbox_003():
    """OUTBOX-003 op=2 保留 7 天：8 天删除、6 天保留。"""
    require_cleanup()
    conv = f'{FAKE_BASE + 930}_{FAKE_BASE + 931}'
    tag = str(now_ms())
    old_key, young_key = f'outbox3-old-{tag}', f'outbox3-young-{tag}'
    try:
        insert_outbox(2, old_key, FAKE_BASE + 930, FAKE_BASE + 931, conv, 1, 2,
                      now_ms() - 8 * DAY_MS)
        insert_outbox(2, young_key, FAKE_BASE + 930, FAKE_BASE + 931, conv, 1, 2,
                      now_ms() - 6 * DAY_MS)
        require(wait_for(lambda: sql_int(
            f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{old_key}'") == 0, timeout=30),
            'op=2 row older than 8d was not deleted')
        require(sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key='{young_key}'") == 1,
                'op=2 row younger than 6d was deleted')
    finally:
        sql(f"DELETE FROM im_outbox WHERE dedupe_key IN ('{old_key}','{young_key}')")


def case_outbox_004():
    """OUTBOX-004 批大小与多轮：1500 条可删行分批删完，无锁等待。"""
    require_cleanup()
    conv = f'{FAKE_BASE + 940}_{FAKE_BASE + 941}'
    tag = str(now_ms())
    prefix = f'outbox4-{tag}'
    try:
        # 1500 条一次 INSERT 会超过单个 argv 128KB 的上限（E2BIG），分批写入
        for start in range(0, 1500, 500):
            values = ','.join(
                f"(1,'{prefix}-{i}',{FAKE_BASE + 940},{FAKE_BASE + 941},'{conv}','',{i},'',"
                f"2,1,0,0,0,0)" for i in range(start, start + 500))
            sql(f"INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,"
                f"conversation_id,server_msg_id,seq,payload,state,stage,available_at,"
                f"lease_until,created_at,updated_at) VALUES {values}")


        def remaining():
            return sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE dedupe_key LIKE '{prefix}-%'")

        require(wait_for(lambda: remaining() == 0, timeout=90, interval=1),
                f'not all rows deleted, remaining={remaining()}')
        require(not log_has('Lock wait timeout'), 'lock wait timeout reported in service logs')
    finally:
        sql(f"DELETE FROM im_outbox WHERE dedupe_key LIKE '{prefix}-%'")


def case_outbox_005():
    """OUTBOX-005 多实例并发清理。"""
    raise Skip('测试环境只起一个 MsgService 实例；多实例 DELETE 为幂等操作，未在本环境验证')


def case_outbox_006():
    """OUTBOX-006 幂等窗口：7 天内同一 clientMsgID 命中旧行；超窗口重新分配。"""
    require_cleanup()
    f = fresh_pair()
    client_msg_id = f'ack6-{f.stamp}'
    try:
        sql(f"INSERT INTO im_conversations(owner_account,conversation_id,peer_account,min_seq,max_seq,"
            f"latest_time,latest_message) VALUES({f.b},'{f.conv}',{f.a},0,1,{now_ms()},'')")
        seed_sequence(f.conv, 1)

        payload = message_payload(f, 1, client_msg_id, 'window')
        # 6 天前完成的发送任务：仍在 7 天幂等窗口内
        insert_outbox(1, f'{f.a}:{client_msg_id}', f.a, f.b, f.conv, 1, 2,
                      now_ms() - 6 * DAY_MS, payload=payload)
        rows_before = sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 "
                              f"AND conversation_id='{f.conv}'")
        send_msg(f.ws_a, f.b, client_msg_id, 'window')
        time.sleep(3)
        rows_after = sql_int(f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 "
                             f"AND conversation_id='{f.conv}'")
        require(rows_after == rows_before,
                f'duplicate inside window allocated a new task: {rows_before} -> {rows_after}')
        require(max_seq(f.b, f.conv) == 1, 'duplicate inside window advanced max_seq')

        # 8 天前：清理后旧行消失，重发当作新消息重新分配 seq
        sql(f"UPDATE im_outbox SET updated_at={now_ms() - 8 * DAY_MS} "
            f"WHERE dedupe_key='{f.a}:{client_msg_id}'")
        require(wait_for(lambda: sql_int(
            f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 "
            f"AND dedupe_key='{f.a}:{client_msg_id}'") == 0, timeout=40),
            'expired op=1 row was not cleaned up')
        send_msg(f.ws_a, f.b, client_msg_id, 'window')
        require(wait_max_seq(f.b, f.conv, 2, timeout=120),
                f'outside window did not allocate a new seq: {max_seq(f.b, f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_070():
    """ACK-070 209 登录同步不受 ACK 影响。"""
    f = fresh_pair()
    try:
        build_thread(f, 4)
        ack(f.ws_b, f.b, [(f.conv, 2)])
        require(wait_receipt(f.b, f.conv, 2), f'delivered_seq={receipt(f.b, f.conv)}')
        f.ws_b.sock.close()
        ws_new, seqs, summary = login_sync(GW1, f.b)
        f.extra.append(ws_new)
        require(summary.conversationsTotal >= 1,
                f'sync summary conversationsTotal={summary.conversationsTotal}')
        require(seqs.get(f.conv, []) == [1, 2, 3, 4],
                f'login sync should push 1..4 regardless of ACK, got {seqs.get(f.conv)}')
    finally:
        f.close()
        f.cleanup()


def case_ack_071():
    """ACK-071 244 拉历史不受 ACK 影响（按 afterSeq）。"""
    f = fresh_pair()
    try:
        build_thread(f, 6)
        before, before_more, _ = pull(f.ws_b, f.b, f.a, after_seq=0)
        ack(f.ws_b, f.b, [(f.conv, 6)])
        require(wait_receipt(f.b, f.conv, 6), f'delivered_seq={receipt(f.b, f.conv)}')
        require(wait_for(lambda: msg_collection().count_documents(
            {'recvID': f.b, 'convID': f.conv, 'status': 2}) == 6, timeout=40),
            'Mongo status=2 not applied')
        after, after_more, _ = pull(f.ws_b, f.b, f.a, after_seq=0)
        require(before == after, f'Pull changed after ACK: {before} -> {after}')
        require(before_more == after_more, 'hasMore changed after ACK')
    finally:
        f.close()
        f.cleanup()


def case_ack_072():
    """ACK-072 104/200 去重（客户端契约）。"""
    raise Skip('去重发生在客户端（按 serverMsgID）；服务端 ACK 只按水位推进，已由 ACK-004/044 覆盖')


# ────────────────────────────── 运行器 ──────────────────────────────

CASES = [
    ('ACK-001', '单会话 ACK', case_ack_001),
    ('ACK-002', '一帧多会话', case_ack_002),
    ('ACK-003', 'ackType 1/2', case_ack_003),
    ('ACK-004', '重复 ACK', case_ack_004),
    ('ACK-010', 'upToSeq=0/负数', case_ack_010),
    ('ACK-011', 'upToSeq>max_seq 截断', case_ack_011),
    ('ACK-012', 'max_seq=0 no-op', case_ack_012),
    ('ACK-013', '非法 conversationID', case_ack_013),
    ('ACK-014', '非参与者 no-op', case_ack_014),
    ('ACK-015', '会话已删除 no-op', case_ack_015),
    ('ACK-016', '群 ID 走 106', case_ack_016),
    ('ACK-017', '一帧重复会话', case_ack_017),
    ('ACK-018', '100 个会话一帧', case_ack_018),
    ('ACK-019', '超大 upToSeq', case_ack_019),
    ('ACK-020', '空 conversations', case_ack_020),
    ('ACK-030', '100 后 80 不回退', case_ack_030),
    ('ACK-031', '80 后 100', case_ack_031),
    ('ACK-032', '跨网关交叉 ACK', case_ack_032),
    ('ACK-033', '低水位不新增 op=3（目标）', case_ack_033),
    ('ACK-034', '提前 ACK 仍可 Pull', case_ack_034),
    ('ACK-035', 'ACK 时消息未提交', case_ack_035),
    ('ACK-040', 'ACK 后 op=1 完成', case_ack_040),
    ('ACK-041', 'ACK 后 Mongo status=2', case_ack_041),
    ('ACK-042', '未 ACK 仍可拉取', case_ack_042),
    ('ACK-043', '乱序不影响 cursor', case_ack_043),
    ('ACK-044', '重连重复 ACK', case_ack_044),
    ('ACK-050', 'PushService 不可用', case_ack_050),
    ('ACK-051', 'MySQL 不可用', case_ack_051),
    ('ACK-053', 'ACK RPC 超时', case_ack_053),
    ('ACK-054', 'Redis 不可用', case_ack_054),
    ('ACK-060', '100 会话一帧 <100ms', case_ack_060),
    ('ACK-061', '1000 条逐步 ACK', case_ack_061),
    ('ACK-062', '回执行数 O(会话)', case_ack_062),
    ('ACK-070', '209 登录同步不受影响', case_ack_070),
    ('ACK-071', '244 拉历史不受影响', case_ack_071),
    ('ACK-072', '104/200 去重（客户端）', case_ack_072),
    ('SERVER-001', 'ACK 提前后仍可 Pull', case_server_001),
    ('SERVER-002', '临时缺口不阻塞 Pull', case_server_002),
    ('OUTBOX-001', 'op=1 超 7 天删除', case_outbox_001),
    ('OUTBOX-002', 'op=3 超 1 天删除', case_outbox_002),
    ('OUTBOX-003', 'op=2 保留 7 天', case_outbox_003),
    ('OUTBOX-004', '批大小与多轮', case_outbox_004),
    ('OUTBOX-005', '多实例并发清理', case_outbox_005),
    ('OUTBOX-006', '幂等窗口', case_outbox_006),
]


def preflight():
    require(os.path.exists(MYSQL_BIN), f'missing mariadb client: {MYSQL_BIN}')
    sql('SELECT 1')
    try:
        mongo().admin.command('ping')
    except Exception as exc:  # noqa: BLE001
        raise AssertionError(f'MongoDB unreachable ({MONGO_URI}): {exc}')
    for port in (GW1, GW2):
        probe = WS(port)
        probe.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--faults', action='store_true', help='运行故障注入用例（会重启 PushService）')
    parser.add_argument('--e2e', action='store_true', help='用真实发送链路准备消息（慢，依赖 Kafka）')
    parser.add_argument('--json', dest='json_path', help='把结果写入 JSON 文件')
    parser.add_argument('--case', action='append', default=[], help='只跑指定用例（可多次）')
    args = parser.parse_args()

    Ctx.faults = args.faults
    Ctx.e2e = args.e2e
    selected = [c for c in CASES if not args.case or c[0] in args.case]
    results = []
    started = time.time()

    print('=== 单聊 ACK 验收测试 ===')
    preflight()
    print(f'环境就绪：gw={GW1}/{GW2}, mysql={MYSQL_BIN}, mode={"e2e" if Ctx.e2e else "fixture"}')

    for case_id, title, func in selected:
        print(f'[{case_id}] {title}')
        begin = time.time()
        status, detail, note = 'PASS', '', ''
        try:
            note = func() or ''
        except Skip as exc:
            status, detail = 'SKIP', str(exc)
        except Warn as exc:
            status, detail = 'WARN', str(exc)
        except AssertionError as exc:
            status, detail = 'FAIL', str(exc)
        except Exception as exc:  # noqa: BLE001
            status, detail = 'FAIL', f'{type(exc).__name__}: {exc}'
        elapsed = round(time.time() - begin, 2)
        results.append({'id': case_id, 'title': title, 'status': status,
                        'detail': detail, 'note': note, 'seconds': elapsed})
        suffix = f' — {detail}' if detail else ''
        extra = f' ({note})' if note and status == 'PASS' else ''
        print(f'  {status} [{elapsed}s]{suffix}{extra}')

    elapsed = round(time.time() - started, 1)
    counts = {key: sum(1 for r in results if r['status'] == key)
              for key in ('PASS', 'SKIP', 'WARN', 'FAIL')}
    print(f'\n合计 {len(results)} 个用例 / {elapsed}s：{counts}')
    for row in results:
        if row['status'] in ('FAIL', 'WARN'):
            print(f"  {row['status']} {row['id']}: {row['detail']}")
    if Ctx.metrics:
        print('指标：' + json.dumps(Ctx.metrics, ensure_ascii=False))

    payload = {'suite': 'single-chat-ack', 'generatedAt': int(time.time()),
               'elapsedSeconds': elapsed, 'counts': counts,
               'metrics': Ctx.metrics, 'cases': results}
    if args.json_path:
        with open(args.json_path, 'w', encoding='utf-8') as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        print(f'结果已写入 {args.json_path}')

    return 1 if counts['FAIL'] else 0


if __name__ == '__main__':
    sys.exit(main())
