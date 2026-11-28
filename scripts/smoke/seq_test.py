"""验证单聊 seq：每会话稠密、双向共用、幂等不重复分配。

不等待服务端回执（Kafka 不在时回执会被拖慢），只发帧后轮询 MySQL 校验。
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sdkws_pb2  # noqa: E402
import group_test  # noqa: E402

MYSQL = '/root/ape-server/build/group-test-tools/root/usr/bin/mariadb'
DB = ['--no-defaults', '--host=127.0.0.1', '--port=3306', '--user=root', 'ape_auth']


def sql(query):
    out = subprocess.run([MYSQL, *DB, '-N', '-e', query], capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError(out.stderr)
    return out.stdout.strip()


def make_friends(a, b):
    sql(f"INSERT IGNORE INTO friends(owner_account,friend_account,created_time) "
        f"VALUES ({a},{b},0),({b},{a},0)")


def send(ws, peer, client_msg_id, content):
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


def wait_rows(conv, expected, timeout=180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        count = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 AND conversation_id='{conv}'"))
        if count >= expected:
            return count
        time.sleep(1)
    return int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 AND conversation_id='{conv}'"))




def ack(ws, user, conversation, up_to):
    req = sdkws_pb2.SdkWSReq()
    req.type = 106
    req.requestId = 'ack-' + conversation
    body = sdkws_pb2.AckReq()
    body.userID = user
    body.ackType = 2
    item = body.conversations.add()
    item.conversationID = conversation
    item.upToSeq = up_to
    req.data = body.SerializeToString()
    ws.send(req.SerializeToString())


def wait_value(query, expected, timeout=20):
    deadline = time.time() + timeout
    value = None
    while time.time() < deadline:
        value = sql(query)
        if value == expected:
            return value
        time.sleep(0.5)
    return value

def main():
    stamp = str(int(time.time()))[-8:]
    a = group_test.register(6666, '133' + stamp)
    b = group_test.register(6666, '134' + stamp)
    print('  ok: registered', a, b)

    ws_a = group_test.login(6666, a)
    ws_b = group_test.login(6666, b)
    make_friends(a, b)

    conv = f'{min(a, b)}_{max(a, b)}'
    for i in range(1, 6):
        send(ws_a, b, f'sq-a-{i}-{stamp}', f'a{i}')
        send(ws_b, a, f'sq-b-{i}-{stamp}', f'b{i}')

    count = wait_rows(conv, 10)
    assert count == 10, f'expected 10 outbox rows, got {count}'

    seqs = [int(x) for x in sql(
        f"SELECT seq FROM im_outbox WHERE operation=1 AND conversation_id='{conv}' ORDER BY id").split()]
    print('  seqs =', seqs)
    assert seqs == list(range(1, 11)), f'expected 1..10, got {seqs}'

    # 幂等：同一 clientMsgID 重发，不应新增 outbox 行 / 新 seq
    send(ws_a, b, f'sq-a-1-{stamp}', 'a1')
    time.sleep(5)
    count = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=1 AND conversation_id='{conv}'"))
    assert count == 10, f'duplicate clientMsgID allocated again: {count}'
    print('  ok: duplicate clientMsgID did not allocate a new seq')

    # ── ACK：按会话水位确认，一次 RPC ──
    # 发送任务的提交（phase true）受 outbox worker 限制，ACK 会被 max_seq 截断；
    # 因此持续上报同一水位，直到提交推进到 10。
    deadline = time.time() + 120
    receipt = ''
    while time.time() < deadline:
        ack(ws_b, b, conv, 10)
        time.sleep(1.5)
        receipt = sql(
            f"SELECT delivered_seq FROM im_conversation_receipts WHERE owner_account={b} "
            f"AND conversation_id='{conv}'")
        if receipt == '10':
            break
    assert receipt == '10', f'receipt watermark={receipt!r}'
    print('  ok: conversation receipt watermark = 10')

    import pymongo
    client = pymongo.MongoClient('mongodb://root:root@localhost:27017/?authSource=admin')
    collection = client['IM-System']['msg']
    deadline = time.time() + 30
    delivered = 0
    while time.time() < deadline:
        delivered = collection.count_documents(
            {'recvID': b, 'convID': conv, 'seq': {'$lte': 10}, 'status': 2})
        if delivered == 5:
            break
        time.sleep(0.5)
    assert delivered == 5, f'Mongo status=2 count={delivered}, expected 5'
    print('  ok: Mongo marked 5 received messages as delivered')

    deadline = time.time() + 30
    while time.time() < deadline:
        if int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}' "
                   f"AND state=2")) >= 1:
            break
        time.sleep(0.5)
    assert int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}' "
                   f"AND state=2")) >= 1, 'ACK cleanup task did not complete'
    print('  ok: ACK cleanup task completed')

    # 幂等：同一水位重复 ACK，不产生新任务
    before = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}'"))
    ack(ws_b, b, conv, 10)
    time.sleep(3)
    after = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}'"))
    assert after == before, f'duplicate ACK created {after - before} tasks'
    print('  ok: duplicate ACK is idempotent')

    # ── 过期 ACK：会话删除后 ACK 应为 no-op，连接保持 ──
    sql(f"DELETE FROM im_conversation_receipts WHERE owner_account={b} AND conversation_id='{conv}'")
    sql(f"DELETE FROM im_conversations WHERE conversation_id='{conv}'")
    op3_before = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}'"))
    ack(ws_b, b, conv, 10)
    time.sleep(1)
    op3_after = int(sql(f"SELECT COUNT(*) FROM im_outbox WHERE operation=3 AND conversation_id='{conv}'"))
    assert op3_after == op3_before, f'expired ACK created op3 tasks: {op3_before} -> {op3_after}'
    receipts = int(sql(f"SELECT COUNT(*) FROM im_conversation_receipts WHERE owner_account={b} "
                       f"AND conversation_id='{conv}'"))
    assert receipts == 0, f'expired ACK wrote receipt: {receipts}'

    ping = sdkws_pb2.SdkWSReq()
    ping.type = 0
    ping.requestId = 'ping-after-stale-ack'
    ws_b.send(ping.SerializeToString())
    got_pong = False
    deadline = time.time() + 5
    while time.time() < deadline:
        frame = ws_b.recv_proto(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            break
        if frame.type == 0 and frame.requestId == 'ping-after-stale-ack':
            got_pong = True
            break
    assert got_pong, 'connection closed by expired ACK'
    print('  ok: expired ACK is no-op and keeps connection')

    print('SEQ TEST PASSED')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('SEQ TEST FAILED:', exc)
        sys.exit(1)
