"""真实进程 + 本地接口：分页离线补取、静默、图片、重启和实时消息。"""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from urllib.request import Request, urlopen

from test_image_understanding import Stub, PNG, image, segment, wait_group_done


class HistoryStub(Stub):
    pages = []
    history = {}
    blocked = threading.Event()
    release = threading.Event()
    pause = False
    failing = False

    def do_POST(self):
        if self.path not in ("/get_group_list", "/get_group_msg_history"):
            return super().do_POST()
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/get_group_list":
            return self.respond({"status": "ok", "data": [{"group_id": 7}, {"group_id": 8}]})
        self.pages.append(data)
        group = str(data["group_id"])
        if self.failing and group == "8":
            return self.respond({"status": "failed"})
        if self.pause and group == "7" and "message_seq" not in data:
            self.blocked.set()
            assert self.release.wait(15), "test release timeout"
        messages = self.history[group]
        end = len(messages)
        if "message_seq" in data:
            end = next(i + 1 for i, m in enumerate(messages)
                       if str(m["message_id"]) == data["message_seq"])
        # 包含游标本身、短页、倒序；不能靠页大小猜同步完成。
        size = 9 if "message_seq" in data else 20
        batch = messages[max(0, end - size):end]
        self.respond({"status": "ok", "data": {"messages": list(reversed(batch))}})


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.05)
    raise AssertionError("condition timed out")


def event(mid, timestamp, group=7, picture=False):
    return {"post_type": "message", "message_type": "group", "user_id": 1,
            "group_id": group, "message_id": mid, "time": timestamp,
            "sender": {"nickname": "测试用户"},
            "message": [segment("text", text=f"触发词 {mid}"), segment("at", qq="99")]
                       + ([image()] if picture else [])}


def run(executable, server_port):
    now = int(time.time())
    HistoryStub.history = {
        "7": [event(1, now - 90000)] + [event(i, now - 1000 + i, picture=i == 30) for i in range(2, 57)],
        "8": [event(101, now - 90000, 8), event(102, now - 300, 8)],
    }
    HistoryStub.pause = True
    HistoryStub.failing = False
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="qq-startup-test-") as directory:
        root = Path(directory)
        config = {"napcat_http_base": f"http://127.0.0.1:{server_port}", "http_report_port": port,
                  "bot_qq": "99", "group_free_talk": [7, 8], "group_trigger_keywords": ["触发词"],
                  "group_history_limit": 200, "group_interaction_history_limit": 100,
                  "ai": {"api_key": "local", "base_url": f"http://127.0.0.1:{server_port}",
                         "model": "text-test", "vision_model": "vision-test"}}
        (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
        history_path = root / "userdata/group_history.json"

        def saved():
            return json.loads(history_path.read_text(encoding="utf-8"))

        def send(value):
            request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(value).encode(),
                              headers={"Content-Type": "application/json"})
            with urlopen(request, timeout=15) as response:
                response.read()

        def start(output):
            process = subprocess.Popen([str(executable)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
            def ready():
                assert process.poll() is None, "bot exited"
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            wait_for(ready)
            return process

        with (root / "process.log").open("wb") as output:
            process = start(output)
            try:
                assert HistoryStub.blocked.wait(10)
                send(event(9000, int(time.time())))
                wait_group_done(root, 9000)
                assert len(Stub.sent) == 1, "同步期间实时消息仍应回复"
                HistoryStub.release.set()
                wait_for(lambda: saved()["groups"]["8"]["synced_until"] >= now)
                groups = saved()["groups"]
                incoming = [m for m in groups["7"]["messages"] if m.get("message_id")]
                ids = [m["message_id"] for m in incoming]
                assert set(ids) == {str(i) for i in range(2, 57)} | {"9000"}
                assert len(ids) == len(set(ids)), "重叠页不能重复入库"
                assert [m["time"] for m in incoming] == sorted(m["time"] for m in incoming)
                assert len(HistoryStub.pages) > 3, "必须跨多页补全超过 20 条消息"
                assert len(Stub.sent) == 1, "历史 @、关键词和自由聊天群不得发言"
                assert len(Stub.requests) == 2, "只允许实时聊天和单独图片描述，不调用历史聊天判断"
                picture = next(m for m in incoming if m["message_id"] == "30")["images"][0]
                assert picture["base64"] == PNG and picture["description"] and picture["id"].isdigit()
                assert (root / "userdata/image_cache/7" / (picture["id"] + ".b64")).read_text() == PNG
                send(HistoryStub.history["7"][29])
                send(event(9000, int(time.time())))
                assert len(Stub.sent) == 1 and len(Stub.requests) == 2, "重复推送必须静默去重"
                checkpoint = groups["7"]["synced_until"]
            finally:
                HistoryStub.release.set()
                process.terminate()
                process.wait(timeout=5)

            # 下一次启动：失败群不推进起点；已存图片不重复描述。
            HistoryStub.history["7"].append(event(8000, checkpoint))
            wait_for(lambda: int(time.time()) > checkpoint)
            HistoryStub.pause = False
            HistoryStub.failing = True
            HistoryStub.pages.clear()
            process = start(output)
            try:
                wait_for(lambda: any(str(p["group_id"]) == "8" for p in HistoryStub.pages))
                assert saved()["groups"]["8"]["synced_until"] == groups["8"]["synced_until"]
                assert len(Stub.sent) == 1 and len(Stub.requests) == 2
                assert saved()["groups"]["7"]["synced_until"] > checkpoint
                assert any(m.get("message_id") == "8000"
                           for m in saved()["groups"]["7"]["messages"]), "重启应补入新离线消息"
                # 失败群保留旧边界，延迟上报中的坏图片只记占位，不发群错误。
                late = event(9999, checkpoint, group=8)
                late["message"] += [segment("image", file="missing")]
                send(late)
                assert len(Stub.sent) == 1
                assert any(m.get("message_id") == "9999" and "读取失败" in m["text"]
                           for m in saved()["groups"]["8"]["messages"])
                send(event(9001, int(time.time())))
                wait_group_done(root, 9001)
                assert len(Stub.sent) == 2, "历史接口失败不影响正常聊天"
            except Exception:
                output.flush()
                print((root / "process.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                process.terminate()
                process.wait(timeout=5)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), HistoryStub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        run(Path(sys.argv[1]).resolve(), server.server_port)
        print("PASS: paginated offline backfill, overlapping short pages, silent image history, live messages, restart dedup and API failure")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
