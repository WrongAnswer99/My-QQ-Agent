"""真实进程 + 本地桩：模型阻塞期间收消息、批量选择、严格目标校验和本地引用。"""
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

from test_image_understanding import Stub, batch_reply, request_text, segment, wait_group_done
from test_startup_history import wait_for


class BatchStub(Stub):
    entered = threading.Event()
    release = threading.Event()
    block = False
    decision_mode = "select"
    reply_mode = "normal"
    fail_send = False
    decisions = []
    generations = []
    quote_target = ""

    def do_POST(self):
        if self.path == "/send_group_msg":
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.fail_send:
                return self.respond({"status": "failed", "retcode": 1})
            self.sent.append(data)
            return self.respond({"status": "ok", "data": {"message_id": 100000 + len(self.sent)}})
        if self.path != "/chat/completions":
            return super().do_POST()
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.requests.append(data)
        context = json.loads(request_text(data))
        decision = "【自由聊天判断协议】" in data["messages"][0]["content"]
        if decision:
            self.decisions.append(context)
            ids = [item["id"] for item in context["pending_messages"]
                   if "接话" in item["content"]]
            if self.decision_mode == "invalid":
                ids = ["other-group-or-history-id"]
            elif self.decision_mode == "duplicate":
                ids = ids * 2
            elif self.decision_mode == "fail":
                return self.respond({"error": {"message": "decision failed"}})
            content = json.dumps({"reply_ids": ids})
        else:
            self.generations.append(context)
            if self.block:
                type(self).block = False
                self.entered.set()
                assert self.release.wait(15), "test release timeout"
            content = batch_reply(data, "接住了 [@123456789]")
            if self.reply_mode == "text":
                content = "不是结构化回复，不能直接发到群里"
            elif self.reply_mode == "unknown":
                content = '{"replies":[{"target_id":"unknown","text":"不该发送"}]}'
            elif self.reply_mode == "missing":
                content = '{"replies":[]}'
            elif self.reply_mode == "duplicate":
                reply = json.loads(content)
                reply["replies"] *= 2
                content = json.dumps(reply)
            elif self.reply_mode in ("quote", "history_quote", "bad_quote", "double_quote", "only_quote", "quote_some"):
                reply = json.loads(content)
                for index, item in enumerate(reply["replies"]):
                    if self.reply_mode == "quote_some" and index > 0:
                        continue
                    quote = item["target_id"] if self.reply_mode != "history_quote" else self.quote_target
                    if self.reply_mode == "bad_quote":
                        quote = "unknown-or-other-group"
                    marker = f"[&{quote}]"
                    item["text"] = marker + ("" if self.reply_mode == "only_quote" else item["text"])
                    if self.reply_mode == "double_quote":
                        item["text"] = marker + item["text"]
                content = json.dumps(reply)
            elif self.reply_mode in ("merge", "skip_all", "skip_invalid", "skip_overlap"):
                reply = json.loads(content)
                if self.reply_mode == "merge":
                    keep = context["messages"][-1]["id"]
                    reply["replies"] = [item for item in reply["replies"] if item["target_id"] == keep]
                    reply["skipped_ids"] = [target for target in context["reply_targets"] if target != keep]
                elif self.reply_mode == "skip_all":
                    reply = {"replies": [], "skipped_ids": context["reply_targets"]}
                else:
                    reply["skipped_ids"] = ["unknown" if self.reply_mode == "skip_invalid"
                                            else context["reply_targets"][0]]
                content = json.dumps(reply)
        self.respond({"choices": [{"message": {"content": content}}]})


def run(executable, server_port):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="qq-batch-") as directory:
        root = Path(directory)
        config = {"napcat_http_base": f"http://127.0.0.1:{server_port}", "http_report_port": port,
                  "bot_qq": "99", "group_free_talk": [7], "group_trigger_keywords": ["触发词"],
                  "group_history_limit": 300, "group_interaction_history_limit": 100,
                  "startup_history_sync_enabled": False, "image_understanding_enabled": False,
                  "ai": {"api_key": "local", "base_url": f"http://127.0.0.1:{server_port}",
                         "model": "test", "system_prompt": "旧规则：只回答最后一条，只输出正文。",
                         "user_prompt": "自然幽默"}}
        (root / "config.json").write_text(json.dumps(config), encoding="utf-8")
        with (root / "process.log").open("wb") as output:
            process = subprocess.Popen([str(executable)], cwd=root, stdout=output, stderr=subprocess.STDOUT)
            try:
                def ready():
                    assert process.poll() is None, "机器人提前退出"
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            return True
                    except OSError:
                        return False
                wait_for(ready)

                def send(mid, text, *, group=7, at=False, self_message=False, sent_event=False):
                    event = {"post_type": "message", "message_type": "group", "group_id": group,
                             "user_id": mid % 3 + 1, "message_id": mid,
                             "message": [segment("text", text=text)] + ([segment("at", qq="99")] if at else [])}
                    if self_message:
                        event["user_id"] = 99
                    if sent_event:
                        event["post_type"] = "message_sent"
                    request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(event).encode(),
                                      headers={"Content-Type": "application/json"})
                    with urlopen(request, timeout=2) as response:
                        response.read()

                def hold(mid, group):
                    BatchStub.entered.clear()
                    BatchStub.release.clear()
                    BatchStub.block = True
                    send(mid, "先回答这条", group=group, at=True)
                    assert BatchStub.entered.wait(5)

                # 当前轮未完成时立即确认新事件；不同群也不被阻塞。
                hold(100, 7)
                start = time.monotonic()
                send(101, "接话：游戏怎么选？")
                send(102, "这条只是背景")
                send(103, "直接提问", at=True)
                send(104, "触发词：再推荐一个")
                send(101, "接话：游戏怎么选？")  # 在途重复推送也不能入队两次。
                assert time.monotonic() - start < 2, "接收不应等待模型完成"
                send(200, "另一个群", group=8, at=True)
                wait_group_done(root, 200)
                assert not BatchStub.decisions, "只有直接触发时不调用选择模型"
                BatchStub.release.set()
                for mid in range(100, 105):
                    wait_group_done(root, mid)
                assert len(BatchStub.decisions) == 1
                pending = BatchStub.decisions[-1]["pending_messages"]
                assert len(pending) == 4 and all(item["status"] == "待处理" for item in pending)
                assert len({item["id"] for item in pending}) == 4
                assert [item["must_reply"] for item in pending] == [False, False, True, True]
                assert len(BatchStub.generations[-1]["reply_targets"]) == 3
                assert len(BatchStub.generations[-1]["messages"]) == 4
                assert len(BatchStub.sent) == 5
                for sent in BatchStub.sent:
                    assert all(part["type"] != "reply" for part in sent["message"]), "无标记默认不引用"
                    assert sent["message"][-1] == segment("at", qq="123456789")
                assert wait_group_done(root, 102)["processing_status"] == "已判断不回复"
                before = len(BatchStub.sent)
                send(101, "重复推送完成消息")
                time.sleep(0.1)
                assert len(BatchStub.sent) == before

                # 非 freetalk：合并多个目标，但绝不询问普通消息是否该回复。
                decisions_before = len(BatchStub.decisions)
                hold(300, 8)
                send(301, "无触发，仅作背景", group=8)
                send(302, "直接问题", group=8, at=True)
                send(303, "触发词问题", group=8)
                BatchStub.release.set()
                for mid in range(300, 304):
                    wait_group_done(root, mid)
                assert len(BatchStub.decisions) == decisions_before
                assert len(BatchStub.generations[-1]["reply_targets"]) == 2
                assert len(BatchStub.generations[-1]["messages"]) == 3

                # 成功回复（含非艾特回复）带真实平台 ID 入历史；回显不重复记录、不再触发 AI。
                saved = json.loads((root / "userdata/group_history.json").read_text(encoding="utf-8"))
                bot_replies = saved["groups"]["7"]["replies"]
                assert len(bot_replies) == 4 and all(item["message_id"] for item in bot_replies)
                echo = bot_replies[-1]
                before_requests = len(BatchStub.requests)
                send(int(echo["message_id"]), echo["text"], self_message=True)
                send(880, "我之前已经推荐过合作游戏", self_message=True, sent_event=True)
                wait_group_done(root, 880)
                assert len(BatchStub.requests) == before_requests
                send(881, "接话：那游戏适合几个人？")
                wait_group_done(root, 881)
                assert "我之前已经推荐过合作游戏" in json.dumps(BatchStub.decisions[-1], ensure_ascii=False)
                assert "我之前已经推荐过合作游戏" in json.dumps(BatchStub.requests[-1], ensure_ascii=False)

                # 单条也遵守按需引用：可引用当前目标、当前群历史中的用户或机器人消息。
                BatchStub.reply_mode = "quote"
                send(890, "触发词：需要明确指向", group=8)
                entry = wait_group_done(root, 890)
                assert BatchStub.sent[-1]["message"][0] == segment("reply", id="890")
                assert "[&" not in json.dumps(BatchStub.sent[-1])
                BatchStub.quote_target = entry["local_id"]
                BatchStub.reply_mode = "history_quote"
                send(891, "触发词：补答之前的问题", group=8)
                wait_group_done(root, 891)
                assert BatchStub.sent[-1]["message"][0] == segment("reply", id="890")
                BatchStub.quote_target = next(item for item in BatchStub.generations[-1]["quoteable_ids"]
                                             if item.startswith("history-"))
                send(892, "触发词：接自己之前的话", group=8)
                wait_group_done(root, 892)
                assert int(BatchStub.sent[-1]["message"][0]["data"]["id"]) >= 100000
                BatchStub.reply_mode = "quote"
                send(893, "接话：这条自由聊天也需要消歧")
                wait_group_done(root, 893)
                assert BatchStub.sent[-1]["message"][0] == segment("reply", id="893")
                BatchStub.reply_mode = "normal"
                hold(894, 8)
                send(895, "需要消歧的问题", group=8, at=True)
                send(896, "触发词自然接话", group=8)
                BatchStub.reply_mode = "quote_some"
                BatchStub.release.set()
                for mid in (894, 895, 896):
                    wait_group_done(root, mid)
                assert BatchStub.sent[-2]["message"][0] == segment("reply", id="895")
                assert all(part["type"] != "reply" for part in BatchStub.sent[-1]["message"])
                BatchStub.reply_mode = "normal"

                # 多个同话题艾特合并成一条，明确跳过已覆盖项；有新问题仍正常接话。
                hold(900, 8)
                send(901, "同一话题补充一", group=8, at=True)
                send(902, "触发词同一话题补充二", group=8)
                send(903, "同一话题最新进展", group=8, at=True)
                # 已阻塞的请求已构造正文，释放后下一批采用 merge。
                BatchStub.reply_mode = "merge"
                BatchStub.release.set()
                wait_group_done(root, 900)
                assert wait_group_done(root, 901)["processing_status"] == "已跳过重复或过时回复"
                assert wait_group_done(root, 902)["processing_status"] == "已跳过重复或过时回复"
                assert wait_group_done(root, 903)["processing_status"] == "已回复"
                BatchStub.reply_mode = "skip_all"
                before_sent = len(BatchStub.sent)
                send(904, "触发词旧话题已结束", group=8)
                assert wait_group_done(root, 904)["processing_status"] == "已跳过重复或过时回复"
                assert len(BatchStub.sent) == before_sent
                BatchStub.reply_mode = "normal"
                send(905, "触发词独立的新问题", group=8)
                assert wait_group_done(root, 905)["processing_status"] == "已回复"

                # 选择失败、越界或重复编号时，艾特目标仍由本地保留。
                for index, mode in enumerate(("invalid", "duplicate", "fail")):
                    base = 400 + index * 10
                    BatchStub.decision_mode = mode
                    hold(base, 7)
                    send(base + 1, "接话：普通问题")
                    send(base + 2, "艾特不能被模型漏掉", at=True)
                    BatchStub.release.set()
                    assert wait_group_done(root, base + 1)["processing_status"] == "判断失败"
                    assert wait_group_done(root, base + 2)["processing_status"] == "已回复"
                BatchStub.decision_mode = "select"

                # 正式生成格式错误不能把 JSON/错误正文泄漏到群里，后续轮次仍可继续。
                for index, mode in enumerate(("text", "unknown", "missing", "duplicate", "bad_quote",
                                              "double_quote", "only_quote", "skip_invalid", "skip_overlap")):
                    BatchStub.reply_mode = mode
                    before = len(BatchStub.sent)
                    send(500 + index, "触发词错误测试", group=8)
                    assert wait_group_done(root, 500 + index)["processing_status"] == "生成失败"
                    assert len(BatchStub.sent) == before
                BatchStub.reply_mode = "normal"
                BatchStub.fail_send = True
                send(600, "触发词发送失败", group=8)
                assert wait_group_done(root, 600)["processing_status"] == "发送失败"
                BatchStub.fail_send = False
                send(601, "触发词恢复", group=8)
                assert wait_group_done(root, 601)["processing_status"] == "已回复"

                # 超过 50 条的积压分成两批，全部有终态，不丢消息。
                hold(700, 8)
                start_generations = len(BatchStub.generations)
                for mid in range(701, 756):
                    send(mid, "背景消息", group=8, at=mid in (701, 755))
                BatchStub.release.set()
                for mid in range(700, 756):
                    wait_group_done(root, mid)
                assert len(BatchStub.generations) == start_generations + 2
                assert [len(item["messages"]) for item in BatchStub.generations[-2:]] == [50, 5]
                assert "已回复" in json.dumps(BatchStub.requests[-1], ensure_ascii=False)

                # 无平台 ID 时不伪造引用；仍可正常回复。
                before = len(BatchStub.sent)
                event = {"post_type": "message", "message_type": "group", "group_id": 8,
                         "user_id": 1, "message": [segment("text", text="触发词，无平台编号")]}
                request = Request(f"http://127.0.0.1:{port}/", data=json.dumps(event).encode(),
                                  headers={"Content-Type": "application/json"})
                with urlopen(request, timeout=2) as response:
                    response.read()
                wait_for(lambda: len(BatchStub.sent) == before + 1)
                assert all(part["type"] != "reply" for part in BatchStub.sent[-1]["message"])
            except Exception:
                output.flush()
                print((root / "process.log").read_text(encoding="utf-8", errors="replace"))
                raise
            finally:
                BatchStub.release.set()
                process.terminate()
                process.wait(timeout=5)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), BatchStub)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        run(Path(sys.argv[1]).resolve(), server.server_port)
        print("PASS: asynchronous reception, batched selection/generation, local quotes, dedup, failures and bounded batches")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()