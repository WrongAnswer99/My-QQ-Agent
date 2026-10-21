"""本地真实进程验证：5 秒间隔、首次 + 3 次重试、停止刷屏和失败边界保留。"""
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.request import Request, urlopen

from test_image_understanding import Stub, segment, wait_group_done
from test_startup_history import event, wait_for


def run(executable, mode):
    class RetryStub(Stub):
        requests = []
        sent = []
        list_calls = []
        history_calls = defaultdict(list)
        starts = defaultdict(list)

        def do_POST(self):
            if self.path not in ("/get_group_list", "/get_group_msg_history"):
                return super().do_POST()
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/get_group_list":
                self.list_calls.append(time.monotonic())
                if mode == "list_error":
                    return self.respond({"status": "failed", "retcode": 1200})
                if mode == "list_empty":
                    return self.respond({"status": "ok", "data": []})
                if mode == "list_invalid":
                    return self.respond("not a OneBot object")
                return self.respond({"status": "ok", "data": [{"group_id": 7}, {"group_id": 8}, {"group_id": 9}]})
            group = str(data["group_id"])
            self.history_calls[group].append(data)
            if "message_seq" not in data:
                self.starts[group].append(time.monotonic())
            if group == "7":
                # 始终返回相同一页，无法到达时间边界（用户反馈的重复游标情况）。
                return self.respond({"status": "ok", "data": {"messages": [event(70, now - 100)]}})
            if group == "8" and len(self.starts[group]) == 1:
                return self.respond({"status": "failed", "retcode": 1200})
            return self.respond({"status": "ok", "data": {"messages": []}})

    now = int(time.time())
    server = ThreadingHTTPServer(("127.0.0.1", 0), RetryStub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryDirectory(prefix="qq-retry-") as directory:
            root = Path(directory)
            config = {"napcat_http_base": f"http://127.0.0.1:{server.server_port}",
                      "http_report_port": port, "bot_qq": "99", "group_history_limit": 100,
                      "image_understanding_enabled": False,
                      "ai": {"api_key": "local", "base_url": f"http://127.0.0.1:{server.server_port}",
                             "model": "test"}}
            (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
            with (root / "process.log").open("wb") as output:
                process = subprocess.Popen([str(executable)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
                try:
                    def log():
                        return (root / "process.log").read_text(encoding="utf-8")

                    wait_for(lambda: "已重试 3 次" in log(), timeout=25)
                    if mode == "groups":
                        assert len(RetryStub.list_calls) == 1
                        assert len(RetryStub.starts["7"]) == 4, "首次 + 3 次重试"
                        assert len(RetryStub.starts["8"]) == 2, "恢复后不再重试"
                        assert len(RetryStub.starts["9"]) == 1, "成功群只补取一次"
                        timestamps = RetryStub.starts["7"]
                        assert log().count("群 7 补取未完成") == 2, "只输出首次失败和最终失败"
                        assert "分页未返回更早消息" in log()
                        saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                        assert saved["groups"]["7"]["synced_until"] < now - 100
                        assert [entry["message_id"] for entry in saved["groups"]["7"]["messages"]] == ["70"]
                        assert all(saved["groups"][group]["synced_until"] >= now for group in ("8", "9"))
                    else:
                        timestamps = RetryStub.list_calls
                        assert len(timestamps) == 4
                        assert not RetryStub.history_calls
                        if mode == "list_invalid":
                            assert "不是有效的 OneBot JSON" in log()
                        if mode == "list_empty":
                            assert "群列表为空" in log()
                    assert all(4.7 <= b - a < 9 for a, b in zip(timestamps, timestamps[1:])), timestamps
                    before = (len(RetryStub.list_calls), sum(map(len, RetryStub.history_calls.values())))
                    time.sleep(5.5)
                    assert before == (len(RetryStub.list_calls), sum(map(len, RetryStub.history_calls.values()))), \
                        "重试用尽后不得继续访问历史接口"
                    # 停止的是补取线程，不是机器人：实时艾特消息仍可回复。
                    incoming = event(9000, int(time.time()))
                    incoming["message"] = [segment("text", text="继续聊天"), segment("at", qq="99")]
                    request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(incoming).encode(),
                                      headers={"Content-Type": "application/json"})
                    with urlopen(request, timeout=5) as response:
                        response.read()
                    wait_group_done(root, 9000)
                    assert len(RetryStub.sent) == 1
                    assert "30 秒后重试" not in log()
                except Exception:
                    output.flush()
                    print((root / "process.log").read_text(encoding="utf-8", errors="replace"))
                    raise
                finally:
                    process.terminate()
                    process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda mode: run(executable, mode), ("groups", "list_error", "list_empty", "list_invalid")))
    print("PASS: 5-second intervals, exactly 3 retries, quiet failure cutoff, independent groups and live chat after exhaustion")