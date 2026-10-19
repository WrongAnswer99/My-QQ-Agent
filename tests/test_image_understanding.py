"""通过真实机器人进程和本地 HTTP 桩验证图片理解，不调用外部服务。"""
import base64
import json
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen

PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII="
DATA_URL = "data:image/png;base64," + PNG


def segment(kind, **data):
    return {"type": kind, "data": data}


def image(**data):
    return segment("image", **(data or {"file": "base64://" + PNG}))


def image_urls(request):
    return [part["image_url"]["url"]
            for message in request["messages"]
            if isinstance(message["content"], list)
            for part in message["content"] if part["type"] == "image_url"]


class Stub(BaseHTTPRequestHandler):
    requests = []
    sent = []
    fail_description = False
    reply_override = None

    def log_message(self, *_):
        pass

    def respond(self, value, status=200):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/image":
            self.respond({}, 404)
            return
        body = base64.b64decode(PNG)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/chat/completions":
            self.requests.append(data)
            describing = "群聊图片历史记录器" in data["messages"][0]["content"]
            if describing and self.fail_description:
                self.respond({"error": {"message": "模拟描述失败"}})
            else:
                content = "测试图片描述：白色背景中有测试文字。" if describing else (self.reply_override or "图片理解测试回复")
                self.respond({"choices": [{"message": {"content": content}}]})
        elif self.path in ("/send_private_msg", "/send_group_msg"):
            self.sent.append(data)
            self.respond({"status": "ok"})
        elif self.path == "/get_msg":
            self.respond({"status": "ok", "data": {"message": [image()]}})
        elif self.path == "/get_file":
            self.respond({"status": "ok", "data": {"base64": PNG}})
        elif self.path == "/get_image" and data["file"] == "remote-image":
            self.respond({"status": "ok", "data": {
                "url": f"http://127.0.0.1:{self.server.server_port}/image"}})
        else:
            self.respond({"status": "failed"})


def run_session(executable, server_port, *, enabled=True, history=False, vision="vision-test",
                history_limit=100):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="qq-image-test-") as directory:
        root = Path(directory)
        config = {
            "napcat_http_base": f"http://127.0.0.1:{server_port}",
            "http_report_port": port, "bot_qq": "99", "master_qq": "88",
            "image_understanding_enabled": enabled, "image_description_only_enabled": not history,
            "group_history_limit": history_limit,
            "group_interaction_history_limit": history_limit,
            "ai": {"api_key": "local-test", "base_url":
                   f"http://127.0.0.1:{server_port}/chat/completions",
                   "model": "text-test", "vision_model": vision,
                   "local_varibles": {"bot_name": "测试机器人"},
                   "system_prompt": "你是[bot_name] [YYYY-MM-DD HH:MM:SS] [imageX]",
                   "user_prompt": "称呼[bot_name] [unknown]"},
        }
        (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
        with (root / "process.log").open("wb") as output:
            process = subprocess.Popen([str(executable)], cwd=root, stdout=output,
                                       stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while True:
                    if process.poll() is not None:
                        raise AssertionError(f"机器人启动失败: {process.returncode}")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise AssertionError("机器人启动超时")
                        time.sleep(0.05)

                def send(parts, group=False, user=1, group_id=7):
                    event = {"post_type": "message", "message_type": "group" if group else "private",
                             "user_id": user, "group_id": group_id, "message": parts}
                    request = Request(f"http://127.0.0.1:{port}/", method="POST",
                                      data=json.dumps(event).encode(),
                                      headers={"Content-Type": "application/json"})
                    with urlopen(request, timeout=10) as response:
                        response.read()

                start = len(Stub.requests)
                text = segment("text", text="请看图")
                at = segment("at", qq="99")
                if not enabled:
                    send([image()])
                    assert len(Stub.requests) == start, "关闭后不应处理纯图片"
                    send([text, image()])
                    assert not image_urls(Stub.requests[-1])
                    assert Stub.requests[-1]["model"] == "text-test"
                    before = len(Stub.requests)
                    send([image()], group=True)
                    assert len(Stub.requests) == before
                    send([at, text, image()], group=True)
                    assert len(Stub.requests) == before + 1
                    assert not image_urls(Stub.requests[-1])
                    return 4

                if history_limit == 0:
                    send([image()], group=True)
                    assert len(Stub.requests) == start
                    send([at, image()], group=True)
                    assert len(Stub.requests) == start + 1
                    assert image_urls(Stub.requests[-1]) == [DATA_URL]
                    return 2

                expected_model = vision or "text-test"
                local_image = root / "sample.png"
                local_image.write_bytes(base64.b64decode(PNG))
                sources = [image(), image(file=DATA_URL), image(url=DATA_URL),
                           image(file_id="file-id"), image(path=str(local_image)),
                           image(url=f"http://127.0.0.1:{server_port}/image"),
                           image(file=f"http://127.0.0.1:{server_port}/image"),
                           image(file="remote-image")]
                for source in sources:
                    send([source])
                    request = Stub.requests[-1]
                    assert request["model"] == expected_model
                    assert image_urls(request) == [DATA_URL], "图片字节或 MIME 不匹配"
                    assert request["messages"][0]["content"] == "你是测试机器人 [YYYY-MM-DD HH:MM:SS] [imageX]"
                    assert request["messages"][1]["content"] == "【用户提示词】\n称呼测试机器人 [unknown]"
                send([text, image(), image()])
                assert len(image_urls(Stub.requests[-1])) == 2
                send([segment("reply", id="123"), text, image()])
                request = Stub.requests[-1]
                assert len(image_urls(request)) == 2
                assert "被引用消息" in request["messages"][-1]["content"][0]["text"]
                before = len(Stub.requests)
                sent_before = len(Stub.sent)
                send([image(), image()], group=True)
                assert len(Stub.sent) == sent_before, "未 @ 不应发送群回复"
                assert len(Stub.requests) == before + (0 if history else 2)
                if not history:
                    for request in Stub.requests[before:]:
                        assert request["model"] == expected_model
                        assert image_urls(request) == [DATA_URL]
                send([at, segment("text", text="刚才图片里有什么？")], group=True)
                if not history:
                    assert not image_urls(Stub.requests[-1]), "后续应只发送历史描述"
                    assert Stub.requests[-1]["model"] == "text-test"
                    assert "测试图片描述" in json.dumps(Stub.requests[-1], ensure_ascii=False)
                else:
                    assert len(image_urls(Stub.requests[-1])) == 2
                send([at, text, image()], group=True)
                assert image_urls(Stub.requests[-1])
                saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                saved_images = [item["images"] for item in saved["groups"]["7"]["messages"]]
                assert any(saved_images) == history, "图片历史开关未生效"
                incoming = saved["groups"]["7"]["messages"][0]
                if not history:
                    assert "图片 1 的自动描述" in incoming["text"]
                    assert "图片 2 的自动描述" in incoming["text"]
                    assert "测试图片描述" in incoming["text"]
                    assert PNG not in json.dumps(saved), "描述模式不应持久化图片 Base64"
                else:
                    assert len(incoming["images"]) == 2
                    assert "自动描述" not in incoming["text"]
                send([at, segment("reply", id="123"), text], group=True)
                assert image_urls(Stub.requests[-1])
                send([text])
                assert Stub.requests[-1]["model"] == "text-test"
                assert not image_urls(Stub.requests[-1])
                before = len(Stub.requests)
                send([text, image(file="nonexistent-image")])
                assert len(Stub.requests) == before, "读取失败不应请求模型"
                assert "图片读取失败" in Stub.sent[-1]["message"]
                send([at, image(file="nonexistent-image")], group=True)
                assert len(Stub.requests) == before
                send([image()], user=88)
                assert len(Stub.requests) == before, "主人消息应保留原有工作流"
                if not history:
                    Stub.fail_description = True
                    try:
                        send([at, text, image()], group=True)
                        assert image_urls(Stub.requests[-1]) == [DATA_URL]
                        assert "群聊图片历史记录器" not in Stub.requests[-1]["messages"][0]["content"]
                        saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                        entry = saved["groups"]["7"]["messages"][-2]
                        assert "描述生成失败" in entry["text"]
                        assert "请看图" in entry["text"]
                        assert not entry["images"]
                    finally:
                        Stub.fail_description = False
                if not history:
                    markers = re.findall(r"\[image\d+\]", incoming["text"])
                    assert len(markers) == 2 and markers[0] != markers[1]
                    Stub.reply_override = "原图：[@123456789]" + "".join(markers)
                    try:
                        send([at, text], group=True)
                        segments = Stub.sent[-1]["message"]
                        assert [part["type"] for part in segments] == ["text", "at", "image", "image"]
                        assert all(part["data"]["file"] == "base64://" + PNG
                                   for part in segments if part["type"] == "image")
                        assert not image_urls(Stub.requests[-1])
                        send([at, text], group=True, group_id=8)
                        assert all(part["type"] != "image" for part in Stub.sent[-1]["message"])
                        assert "原图不可用" in json.dumps(Stub.sent[-1], ensure_ascii=False)
                    finally:
                        Stub.reply_override = None
                return 22 if not history else 19
            except Exception:
                output.flush()
                print((root / "process.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                process.terminate()
                process.wait(timeout=5)


def main():
    executable = Path(sys.argv[1]).resolve()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        checks = sum(run_session(executable, server.server_port, **options) for options in (
            {}, {"history": True}, {"vision": ""}, {"enabled": False, "history": True},
            {"history_limit": 0}))
        print(f"PASS: {checks} image integration scenarios across 5 configurations")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
