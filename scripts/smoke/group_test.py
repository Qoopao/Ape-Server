"""群聊 v3 冒烟测试：250 建群/审批 → 246 发送 → 251 内联推送（两网关）。

v3：群消息正文在 Mongo 单副本；209 只表示单聊同步结束，登录后必须继续收 210；
没有 ACK / 107，未读由客户端本地维护。
"""
import os
import sys
import time

sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import apeauth_pb2  # noqa: E402
import groupchat_pb2  # noqa: E402
import sdkws_pb2  # noqa: E402
from ws_test import WS, expect  # noqa: E402


def register(port, phone):
    ws = WS(port)
    req = sdkws_pb2.SdkWSReq()
    req.type = 90
    req.requestId = 'reg-' + phone
    reg = apeauth_pb2.RegisterRequest()
    reg.phone = phone
    reg.nickname = 'g' + phone[-4:]
    reg.password = 'pw123456'
    req.data = reg.SerializeToString()
    ws.send(req.SerializeToString())
    resp = ws.recv_proto()
    ws.sock.close()
    if resp is None or resp.account == 0:
        raise AssertionError(f'register {phone} failed')
    return resp.account


def login(port, account):
    """兼容其他冒烟脚本：返回已收完 209/210 的 WS。"""
    ws, _ = login_v3(port, account)
    return ws


def login_v3(port, account):
    """登录并收完 209（单聊）→ 251（群内联）→ 210（群同步结束）。"""
    ws = WS(port)
    req = sdkws_pb2.SdkWSReq()
    req.type = 91
    req.account = account
    req.deviceID = 'dev'
    req.requestId = 'login-{}'.format(account)
    req.data = b'pw123456'
    ws.send(req.SerializeToString())
    resp = ws.recv_proto()
    if resp is None or resp.errorCode != '':
        raise AssertionError(f'login {account} failed: {None if resp is None else resp.errorCode}')
    stash = []
    while True:
        frame = ws.recv_proto(timeout=8)
        if frame is None:
            raise AssertionError('missing 210 group sync summary')
        if frame.type == 210:
            break
        if frame.type != 209:
            stash.append(frame)
    return ws, stash


def command(ws, cmd, request_id, stash):
    req = sdkws_pb2.SdkWSReq()
    req.type = 250
    req.requestId = request_id
    req.data = cmd.SerializeToString()
    ws.send(req.SerializeToString())
    deadline = time.time() + 10
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=deadline - time.time())
        if frame is None:
            raise AssertionError(f'no reply for {request_id}')
        if frame.type == 250:
            reply = groupchat_pb2.Reply()
            reply.ParseFromString(frame.data)
            return reply
        stash.append(frame)
    raise AssertionError(f'timeout waiting reply {request_id}')


def send_group(ws, group_id, text, client_id, stash):
    """246：返回 SendGroupMessageResp（errorCode 在内部）。"""
    req = sdkws_pb2.SdkWSReq()
    req.type = 246
    req.requestId = 'send-' + client_id
    command = groupchat_pb2.SendGroupMessageReq(
        groupID=group_id, clientID=client_id, content=text.encode(), contentType=101)
    req.data = command.SerializeToString()
    ws.send(req.SerializeToString())
    deadline = time.time() + 10
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=deadline - time.time())
        if frame is None:
            raise AssertionError(f'no reply for {client_id}')
        if frame.type == 246:
            resp = groupchat_pb2.SendGroupMessageResp()
            resp.ParseFromString(frame.data)
            return resp
        stash.append(frame)
    raise AssertionError(f'timeout waiting 246 for {client_id}')


def wait_group_notice(ws, group_id, content, sender, stash, timeout=20):
    """251：小群内联 sdkws.MsgData；大群只带 lastSeq/epoch。"""
    wanted = content.encode()

    def match(notice):
        return notice.groupID == group_id and any(
            m.content == wanted and m.sendID == sender and m.isGroupMsg
            for m in notice.messages)

    for frame in stash:
        if frame.type == 251:
            notice = groupchat_pb2.Notice()
            notice.ParseFromString(frame.data)
            if match(notice):
                return notice
    deadline = time.time() + timeout
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=deadline - time.time())
        if frame is None:
            break
        if frame.type != 251:
            continue
        notice = groupchat_pb2.Notice()
        notice.ParseFromString(frame.data)
        if match(notice):
            return notice
    raise AssertionError(f'group notice with {content!r} not received')


def main():
    stamp = str(int(time.time()))[-8:]
    a_phone, b_phone = '139' + stamp, '137' + stamp
    a = register(6666, a_phone)
    b = register(6667, b_phone)
    print('  ok: registered A/B', a, b)

    ws_a, stash_a = login_v3(6666, a)
    ws_b, stash_b = login_v3(6667, b)
    print('  ok: A on gw-1, B on gw-2 (209 -> 210)')

    create = groupchat_pb2.Command()
    create.operation = groupchat_pb2.Create
    create.text = 'smoke-group'
    create.clientID = 'create-' + stamp
    reply = command(ws_a, create, 'cmd-create', stash_a)
    expect(reply.errorCode == '0' and reply.groupID != '',
           f'group created id={reply.groupID!r} err={reply.errorCode!r}')
    group_id = reply.groupID

    apply = groupchat_pb2.Command()
    apply.operation = groupchat_pb2.Apply
    apply.groupID = group_id
    apply.text = 'join'
    apply.clientID = 'apply-' + stamp
    reply = command(ws_b, apply, 'cmd-apply', stash_b)
    expect(reply.errorCode == '0', f'B applied err={reply.errorCode!r}')

    listing = groupchat_pb2.Command()
    listing.operation = groupchat_pb2.Applications
    listing.groupID = group_id
    listing.clientID = 'apps-' + stamp
    reply = command(ws_a, listing, 'cmd-apps', stash_a)
    expect(len(reply.applications) > 0, 'owner sees pending application')
    application_id = reply.applications[0].id

    review = groupchat_pb2.Command()
    review.operation = groupchat_pb2.Review
    review.groupID = group_id
    review.applicationID = application_id
    review.accept = True
    review.clientID = 'review-' + stamp
    reply = command(ws_a, review, 'cmd-review', stash_a)
    expect(reply.errorCode == '0', f'B approved err={reply.errorCode!r}')

    text = 'hello-' + stamp
    sent = send_group(ws_a, group_id, text, 'msg-' + stamp, stash_a)
    expect(sent.errorCode == '0', f'group send err={sent.errorCode!r}')
    expect(sent.msg.seq == 1, f'first group message seq={sent.msg.seq}')
    expect(sent.msg.recvID == 0 and sent.msg.sessionType == 2 and sent.msg.isGroupMsg,
           'group MsgData carries recvID=0/sessionType=2/isGroupMsg')

    # 旧 250 Send 已被删除：必须返回 UNSUPPORTED_OPERATION，而不是静默成功。
    old_send = groupchat_pb2.Command()
    old_send.operation = 11
    old_send.groupID = group_id
    old_send.text = 'legacy'
    old_send.clientID = 'legacy-' + stamp
    reply = command(ws_a, old_send, 'cmd-legacy', stash_a)
    expect(reply.errorCode == 'UNSUPPORTED_OPERATION',
           f'old 250 Send rejected err={reply.errorCode!r}')

    # 小群：两个成员都应通过各自网关收到 251 内联正文；发送者本人不收。
    wait_group_notice(ws_b, group_id, text, a, stash_b)
    print('  ok: B received 251 inline body on gw-2')
    for frame in stash_a:
        if frame.type == 251:
            notice = groupchat_pb2.Notice()
            notice.ParseFromString(frame.data)
            expect(not any(m.content == text.encode() for m in notice.messages),
                   'sender does not receive own 251')
            break
    print('  ok: A did not receive its own 251')

    print('GROUP SMOKE PASSED')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('GROUP SMOKE FAILED:', exc)
        sys.exit(1)
