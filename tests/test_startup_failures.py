"""模拟启动接口故障后恢复，以及只有 22 条记录的新群。"""
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

from test_image_understanding import Stub, segment, wait_group_done
from test_startup_history import event, wait_for


class StartupFailureStub(Stub):
    attempts = 0
    history_calls = 0
    messages = []
    token = "local-diagnostic-secret"

    def do_POST(self):
        if self.path not in ("/get_group_list", "/get_group_msg_history"):
            return super().do_POST()
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/get_group_list":
            assert data == {"no_cache": True}, "新建群需要刷新缓存"
            type(self).attempts += 1
            n = self.attempts
            if n == 1:
                return self.respond({}, 401)
            if n == 2:
                return self.respond({"status": "failed", "retcode": 1200,
                                     "message": "not ready " + self.token})
            if n == 3:
                return self.respond({"status": "ok", "data": {}})
            return self.respond({"status": "ok", "data": [{"group_id": 7}]})
        type(self).history_calls += 1
        if "message_seq" not in data:
            batch = self.messages[-20:]
        elif data["message_seq"] == "3":
            batch = self.messages[:3]
        else:
            # 与安装版 NapCat 一致：无历史时以失败响应返回，不是空数组。
            return self.respond({"status": "failed", "retcode": 1200, "message": "消息不存在"})
        return self.respond({"status": "ok", "data": {"messages": batch}})


def run(executable):
    now = int(time.time())
    StartupFailureStub.messages = [event(i, now - 100 + i) for i in range(1, 23)]
    server = ThreadingHTTPServer(("127.0.0.1", 0), StartupFailureStub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryDirectory(prefix="qq-startup-failures-") as directory:
            root = Path(directory)
            config = {"napcat_http_base": f"http://127.0.0.1:{server.server_port}/",
                      "napcat_token": StartupFailureStub.token, "http_report_port": port,
                      "bot_qq": "99", "group_history_limit": 100,
                      "ai": {"api_key": "local", "base_url": f"http://127.0.0.1:{server.server_port}",
                             "model": "test"}}
            (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
            log_path = root / "process.log"
            with log_path.open("wb") as output:
                process = subprocess.Popen([str(executable)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
                try:
                    wait_for(lambda: StartupFailureStub.history_calls >= 3, timeout=30)
                    wait_for(lambda: "补取未完成" in log_path.read_text(encoding="utf-8"))
                    log = log_path.read_text(encoding="utf-8")
                    for expected in ("HTTP 401", "retcode=1200", "[REDACTED]", "data 应为数组",
                                     "消息不存在", "5 秒后重试", "3/3"):
                        assert expected in log, expected
                    assert StartupFailureStub.token not in log, "日志不得泄露令牌"
                    saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                    messages = saved["groups"]["7"]["messages"]
                    assert [m["message_id"] for m in messages] == [str(i) for i in range(1, 23)]
                    assert not Stub.sent and not Stub.requests, "22 条历史 @ 消息必须静默"
                    current = event(1000, int(time.time()))
                    current["message"] = [segment("at", qq="99"), segment("text", text="之前说过什么？")]
                    request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(current).encode(),
                                      headers={"Content-Type": "application/json"})
                    with urlopen(request, timeout=10) as response:
                        response.read()
                    wait_group_done(root, 1000)
                    background = Stub.requests[-1]["messages"][1]["content"]
                    assert all(f"触发词 {i}[@99]" in background for i in range(1, 23)), "模型必须收到 22 条历史"
                    assert len(Stub.sent) == 1
                except Exception:
                    output.flush()
                    print(log_path.read_text(encoding="utf-8", errors="replace"))
                    raise
                finally:
                    process.terminate()
                    process.wait(timeout=5)
        print("PASS: startup API diagnostics, token redaction, refresh/retry recovery and all 22 new-group messages in model context")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve())
