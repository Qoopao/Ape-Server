import argparse
import getpass
import json
import os
from pathlib import Path

import mysql.connector
from pymongo import MongoClient


def summary(message):
    if not message:
        return {}
    text = message.get("content", "")
    if message.get("contentType", 0) == 101:
        try:
            parsed = json.loads(text)
            if isinstance(parsed, dict) and isinstance(parsed.get("content"), str):
                text = parsed["content"]
        except (TypeError, ValueError):
            pass
        text = text.encode("utf-8")[:512].decode("utf-8", errors="ignore")
        preview = json.dumps({"content": text}, ensure_ascii=False)
    else:
        preview = f"[message type {message.get('contentType', 0)}]"
    return {
        "serverMsgID": message.get("serverMsgID", ""),
        "clientMsgID": message.get("clientMsgID", ""),
        "sessionType": 1,
        "sendID": str(message.get("sendID", 0)),
        "recvID": str(message.get("recvID", 0)),
        "senderName": message.get("senderNickname", ""),
        "LatestMsgRecvTime": str(int(message.get("sendTime", 0))),
        "contentType": message.get("contentType", 0),
        "content": preview,
    }


def migrate(args):
    password = os.environ.get("APE_MYSQL_PASSWORD") or getpass.getpass("MySQL password: ")
    mongo = MongoClient(args.mongo_uri)
    connection = mysql.connector.connect(
        host=args.mysql_host, port=args.mysql_port, user=args.mysql_user,
        password=password, database=args.mysql_database, charset="utf8mb4",
        autocommit=False,
    )
    cursor = connection.cursor()
    imported = 0
    skipped = 0
    try:
        schema = Path(__file__).with_name("sql").joinpath("conversations.sql").read_text(encoding="utf-8")
        for statement in schema.split(";"):
            if statement.strip():
                cursor.execute(statement)
        database = mongo["IM-System"]
        database["msg"].create_index([("convID", 1), ("seq", 1)])
        database["msg"].create_index([("convID", 1), ("recvID", 1), ("seq", 1)])
        legacy = database["conversations"].find({"conversationType": 1}).batch_size(200)
        for record in legacy:
            owner = int(record["ownerUserID"])
            peer = int(record["userID"])
            conversation = str(record["conversationID"])
            if conversation != f"{min(owner, peer)}_{max(owner, peer)}" or owner == peer:
                skipped += 1
                continue
            cursor.execute(
                "SELECT COUNT(*) FROM friends WHERE (owner_account=%s AND friend_account=%s) "
                "OR (owner_account=%s AND friend_account=%s)", (owner, peer, peer, owner),
            )
            if cursor.fetchone()[0] != 2:
                connection.rollback()
                skipped += 1
                continue
            cursor.execute(
                "SELECT deleted_seq FROM friend_chat_deletions WHERE owner_account=%s AND peer_account=%s",
                (owner, peer),
            )
            deletion = cursor.fetchone()
            minimum = max(int(record.get("minSeq", 0)), int(deletion[0]) if deletion else 0)
            latest = database["msg"].find_one(
                {"convID": conversation, "isGroupMsg": False, "committed": {"$ne": False}, "seq": {"$gt": minimum}},
                sort=[("seq", -1)],
            )
            maximum = int(latest["seq"]) if latest else 0
            read = min(maximum, max(0, int(record.get("readSeq", 0))))
            cursor.execute(
                "INSERT IGNORE INTO im_conversations(owner_account,conversation_id,peer_account,is_pinned,"
                "recv_msg_opt,min_seq,max_seq,read_seq,latest_time,latest_message) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (owner, conversation, peer, bool(record.get("isPinned", False)), int(record.get("recvMsgOpt", 0)),
                 minimum, maximum, read, int(latest.get("sendTime", 0)) if latest else 0,
                 json.dumps(summary(latest), ensure_ascii=False)),
            )
            if cursor.rowcount != 1:
                connection.rollback()
                skipped += 1
                continue
            batch = []
            messages = database["msg"].find(
                {"convID": conversation, "recvID": owner, "isGroupMsg": False,
                 "committed": {"$ne": False},
                 "seq": {"$gt": max(read, minimum)}}, {"seq": 1, "_id": 0},
            ).batch_size(500)
            for message in messages:
                batch.append((owner, conversation, int(message["seq"])))
                if len(batch) == 500:
                    cursor.executemany(
                        "INSERT IGNORE INTO im_conversation_unread(owner_account,conversation_id,seq) VALUES (%s,%s,%s)", batch,
                    )
                    batch.clear()
            if batch:
                cursor.executemany(
                    "INSERT IGNORE INTO im_conversation_unread(owner_account,conversation_id,seq) VALUES (%s,%s,%s)", batch,
                )
            cursor.execute(
                "UPDATE im_conversations SET unread_count=(SELECT COUNT(*) FROM im_conversation_unread "
                "WHERE owner_account=%s AND conversation_id=%s) WHERE owner_account=%s AND conversation_id=%s",
                (owner, conversation, owner, conversation),
            )
            connection.commit()
            imported += 1
        print(f"Imported {imported} conversations; skipped {skipped} existing or ineligible records.")
    except Exception:
        connection.rollback()
        raise
    finally:
        cursor.close()
        connection.close()
        mongo.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Import legacy MongoDB single-chat conversations into MySQL.")
    parser.add_argument("--server-stopped", action="store_true", required=True,
                        help="Confirm that all server writers are stopped for this migration.")
    parser.add_argument("--mongo-uri", default=os.environ.get("APE_MONGO_URI", "mongodb://localhost:27017"))
    parser.add_argument("--mysql-host", default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, default=3306)
    parser.add_argument("--mysql-user", default="ape_user")
    parser.add_argument("--mysql-database", default="ape_auth")
    migrate(parser.parse_args())
