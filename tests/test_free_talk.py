"""验证群聊触发、自由聊天判断和私聊权限，不调用外部服务。"""
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

from test_image_understanding import Stub, image, image_urls, segment


class TalkStub(Stub):
    decision = '{"reply":false}'
    fail_decision = False
    decisions = []

    def do_POST(self):
        if self.path != "/chat/completions":
            return super().do_POST()
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.requests.append(data)
        if "【自由聊天判断协议】" in data["messages"][0]["content"]:
            self.decisions.append(data)
            if self.fail_decision:
                return self.respond({"error": {"message": "decision unavailable"}})
            content = self.decision
        else:
            content = "普通聊天回复"
        self.respond({"choices": [{"message": {"content": content}}]})


def run(executable, server_port, *, history_limit=100, private=True, master="88"):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="qq-free-talk-") as directory:
        root = Path(directory)
        config = {
            "napcat_http_base": f"http://127.0.0.1:{server_port}", "http_report_port": port,
            "bot_qq": "99", "master_qq": master, "private_chat_enabled": private,
            "group_free_talk": [7], "group_trigger_keywords": ["触发词"],
            "group_need_at": False,  # 旧字段不得恢复全群自动回复
            "group_history_limit": history_limit, "group_interaction_history_limit": history_limit,
            "image_description_only_enabled": False,
            "ai": {"api_key": "local", "base_url": f"http://127.0.0.1:{server_port}/chat/completions",
                   "model": "text-test", "vision_model": "vision-test",
                   "local_varibles": {"bot_name": "测试机器人"}, "user_prompt": "你叫[bot_name]",
                   "free_talk_prompt": "判断是否叫[bot_name] [unknown]"},
        }
        (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
        with (root / "process.log").open("wb") as output:
            process = subprocess.Popen([str(executable)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while True:
                    if process.poll() is not None:
                        raise AssertionError("机器人提前退出")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        assert time.monotonic() < deadline, "启动超时"
                        time.sleep(0.05)

                def send(text="闲聊", *, group=7, user=1, extra=()):
                    event = {"post_type": "message", "message_type": "private" if group is None else "group",
                             "user_id": user, "group_id": group or 0,
                             "message": [segment("text", text=text), *extra]}
                    before = (len(TalkStub.requests), len(TalkStub.decisions), len(TalkStub.sent))
                    request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(event).encode(),
                                      headers={"Content-Type": "application/json"})
                    with urlopen(request, timeout=10) as response:
                        response.read()
                    return tuple(after - prior for after, prior in zip(
                        (len(TalkStub.requests), len(TalkStub.decisions), len(TalkStub.sent)), before))

                assert send(group=8) == (0, 0, 0), "普通群不调用判断"
                assert send(group=8, extra=[segment("at", qq="99")]) == (1, 0, 1)
                assert send("触发词", group=8) == (1, 0, 1)
                TalkStub.decision = '{"reply":false}'
                assert send("别人的话题") == (1, 1, 0)
                assert send(extra=[segment("at", qq="99")]) == (1, 0, 1), "@ 跳过判断"
                assert send("触发词") == (1, 0, 1), "关键词跳过判断"
                TalkStub.decision = '{"reply":true}'
                assert send("测试机器人，你觉得呢？") == (2, 1, 1)
                decision = TalkStub.decisions[-1]
                assert "判断是否叫测试机器人 [unknown]" in decision["messages"][0]["content"]
                assert decision["temperature"] == 0 and decision["model"] == "text-test"
                context = json.loads(decision["messages"][-1]["content"])
                assert context["bot_profile"] == "你叫测试机器人" and context["bot_qq"] == "99"
                assert context["current_message"] == "测试机器人，你觉得呢？"
                assert bool(context["recent_messages"]) == bool(history_limit)
                if history_limit:
                    assert any("普通聊天回复" in item for item in context["recent_messages"])
                    assert len(context["recent_messages"]) == 5, "普通历史与交互历史按序号去重，保留不同消息的相同文本"
                for invalid in ('{"reply":false}', '{"reply":"true"}', 'true', '{}', '不是 JSON',
                                '```json\n{"reply":true}\n```'):
                    TalkStub.decision = invalid
                    assert send() == (1, 1, 0), invalid
                TalkStub.fail_decision = True
                try:
                    assert send() == (1, 1, 0), "判断失败不发送错误到群聊"
                finally:
                    TalkStub.fail_decision = False
                TalkStub.decision = '{"reply":true}'
                assert send(user=99) == (0, 0, 0), "忽略自身消息"
                assert send("继续看图", extra=[image()]) == (2, 1, 1)
                assert not image_urls(TalkStub.decisions[-1]), "判断只使用文本"
                assert image_urls(TalkStub.requests[-1]), "正式回复仍读取图片"
                assert send(extra=[segment("reply", id="123"), segment("at", qq="12345")]) == (2, 1, 1)
                context = json.loads(TalkStub.decisions[-1]["messages"][-1]["content"])
                assert "12345" in context["current_message"]
                assert send(group=None) == ((1, 0, 1) if private else (0, 0, 0))
                if master:
                    assert send(group=None, user=88) == (0, 0, 0), "主人普通消息不走聊天"
                    assert send("/status", group=None, user=88) == (0, 0, 1), "主人指令不受私聊开关影响"
                else:
                    assert send(group=None, user=88) == ((1, 0, 1) if private else (0, 0, 0))
                if history_limit:
                    saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                    assert any("别人的话题" in item["text"] for item in saved["groups"]["7"]["messages"])
            except Exception:
                output.flush()
                print((root / "process.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                process.terminate()
                process.wait(timeout=5)


def main():
    executable = Path(sys.argv[1]).resolve()
    server = ThreadingHTTPServer(("127.0.0.1", 0), TalkStub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for options in ({}, {"history_limit": 0, "private": False}, {"master": ""},
                        {"master": "", "private": False}):
            run(executable, server.server_port, **options)
        print("PASS: free talk triggers, decision failures, image routing and private permissions across 4 configurations")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
