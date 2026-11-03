#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# SPDX-License-Identifier: Apache-2.0
"""Run against a locally built seekdb: SEEKDB_TEST_PORT=2888 python3 this_file.

Uses a loopback OpenAI-compatible echo server; no credentials or external AI.
Creates and removes its own database, model and endpoint.
"""
import http.server
import json
import os
import subprocess
import threading
import unittest
import uuid


def sql_text(value):
    return "CONVERT(0x" + value.encode("utf-8").hex() + " USING utf8mb4)" if value else "''"


class PromptIntegration(unittest.TestCase):
    @classmethod
    def sql(cls, statement, ok=True):
        result = subprocess.run(cls.command, input=statement, text=True,
                                capture_output=True, timeout=90)
        if ok and result.returncode:
            raise AssertionError(result.stderr)
        if not ok and not result.returncode:
            raise AssertionError("Expected SQL failure: " + statement)
        return result.stdout.strip() if ok else result.stderr

    @classmethod
    def setUpClass(cls):
        suffix = uuid.uuid4().hex[:10]
        cls.database = "prompt_test_" + suffix
        cls.model = "prompt_model_" + suffix
        cls.endpoint = "prompt_endpoint_" + suffix
        cls.command = [os.environ.get("SEEKDB_TEST_MYSQL", "mysql"),
                       "-h", "127.0.0.1", "-P", os.environ.get("SEEKDB_TEST_PORT", "2888"),
                       "-u", "root", "--batch", "--raw", "--skip-column-names",
                       "--default-character-set=utf8mb4"]
        cls.requests = []

        class Echo(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                cls.requests.append(body)
                content = next(m["content"] for m in body["messages"] if m["role"] == "user")
                response = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, *_):
                pass

        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)
        cls.addClassCleanup(cls.server.server_close)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.shutdown)
        cls.sql("CREATE DATABASE " + cls.database)
        cls.addClassCleanup(cls.sql, "DROP DATABASE IF EXISTS " + cls.database)
        cls.command.append(cls.database)
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('%s', '%s')" %
                (cls.model, json.dumps({"type": "completion", "model_name": "prompt-echo"})))
        cls.addClassCleanup(cls.sql, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL('%s')" % cls.model)
        config = {"ai_model_name": cls.model, "scope": "all", "provider": "openai",
                  "url": "http://127.0.0.1:%d/v1/chat/completions" % cls.server.server_port,
                  "access_key": "local-test-only"}
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('%s', '%s')" % (cls.endpoint, json.dumps(config)))
        cls.addClassCleanup(cls.sql, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('%s')" % cls.endpoint)

    def render(self, prompt):
        return json.loads(self.sql("SELECT JSON_QUOTE(AI_COMPLETE('%s', %s))" % (self.model, prompt)))

    def test_completion_null_config(self):
        for config in ["NULL", "CAST(NULL AS CHAR)", "CAST(NULL AS JSON)"]:
            with self.subTest(config=config):
                expression = "AI_COMPLETE('%s', 'hello', %s)" % (self.model, config)
                self.assertEqual(self.sql("SELECT LENGTH(%s) > 0 AS complete_ok" % expression), "1")
                self.assertEqual(json.loads(self.sql("SELECT JSON_QUOTE(%s)" % expression)), "hello")
        self.assertEqual(json.loads(self.sql(
            "SELECT JSON_QUOTE(AI_COMPLETE('%s', AI_PROMPT('hello {0}', 'world'), NULL))" % self.model)),
            "hello world")

    def test_completion_string_config(self):
        expression = "AI_COMPLETE('%s', 'hello', '{\"temperature\":0.2,\"max_tokens\":16}')" % self.model
        self.assertEqual(json.loads(self.sql("SELECT JSON_QUOTE(%s)" % expression)), "hello")
        self.assertEqual(self.requests[-1]["temperature"], 0.2)
        self.assertEqual(self.requests[-1]["max_tokens"], 16)
        count = len(self.requests)
        error = self.sql("SELECT AI_COMPLETE('%s', 'hello', 'invalid-json')" % self.model, ok=False)
        self.assertIn("ERROR 3140", error)
        self.assertEqual(len(self.requests), count)

    def test_json_structure(self):
        value = json.loads(self.sql("SELECT AI_PROMPT('{0}/{1}', JSON_ARRAY(1,true,null), JSON_OBJECT('key',2))"))
        self.assertEqual(value, {"template": "{0}/{1}", "args": [[1, True, None], {"key": 2}]})

    def test_repeated_placeholder(self):
        self.assertEqual(self.render("AI_PROMPT(%s, '%s')" % (sql_text("{0}" * 40), "x" * 1000)), "x" * 40000)

    def test_nested_prompt(self):
        self.assertEqual(self.render("AI_PROMPT('outer[{0}] {1}', AI_PROMPT('{1}/{0}/{1}','a','b'),'end')"), "outer[b/a/b] end")

    def test_json_rendering(self):
        for expression, expected in [("JSON_ARRAY(1,true,null)", [1, True, None]),
                                     ("JSON_OBJECT('key',2)", {"key": 2}),
                                     ("CAST('42' AS JSON)", 42),
                                     ("CAST('null' AS JSON)", None)]:
            with self.subTest(expression=expression):
                self.assertEqual(json.loads(self.render("AI_PROMPT('{0}', %s)" % expression)), expected)

    def test_unicode_and_escapes(self):
        value = '\u4e16\u754c "quote" \\ slash\n\t\x00 \U0001f30a'
        self.assertEqual(self.render("AI_PROMPT('before{0}after', %s)" % sql_text(value)), "before" + value + "after")

    def test_argument_is_not_reparsed(self):
        self.assertEqual(self.render("AI_PROMPT('{0}/{1}', '{1}', 'end')"), "{1}/end")

    def test_literal_braces(self):
        self.assertEqual(self.render("AI_PROMPT('{x} {} { unclosed {0}', 'ok')"), "{x} {} { unclosed ok")

    def test_order_and_leading_zero(self):
        self.assertEqual(self.render("AI_PROMPT('{01}/{0}/{1}','a','b')"), "b/a/b")

    def test_nested_depth_limit(self):
        value = {"template": "{0}", "args": ["leaf"]}
        for _ in range(70):
            value = {"template": "{0}", "args": [value]}
        count = len(self.requests)
        self.sql("SELECT AI_COMPLETE('%s', CAST(%s AS JSON))" %
                 (self.model, sql_text(json.dumps(value))), ok=False)
        self.assertEqual(len(self.requests), count)

    def test_invalid_indexes(self):
        for template in ['{1}', '{9999999999999999999999999999999999999}']:
            with self.subTest(template=template):
                count = len(self.requests)
                self.sql("SELECT AI_COMPLETE('%s', AI_PROMPT(%s,'a'))" % (self.model, sql_text(template)), ok=False)
                self.assertEqual(len(self.requests), count)

    def test_null_and_invalid_types(self):
        for expression in ["AI_PROMPT(NULL)", "AI_PROMPT('{0}',NULL)",
                           "AI_PROMPT('{0}',CAST(NULL AS CHAR))", "AI_PROMPT('{0}',1)", "AI_PROMPT('')"]:
            with self.subTest(expression=expression):
                self.sql("SELECT " + expression, ok=False)

    def test_longtext_lob(self):
        self.sql("CREATE TABLE prompt_lob (id INT PRIMARY KEY, template LONGTEXT, value LONGTEXT)")
        try:
            self.sql("INSERT INTO prompt_lob VALUES (1, CONCAT(REPEAT('t',80000),'{0}/{0}'),REPEAT('v',80000))")
            lengths = self.sql("SELECT LENGTH(JSON_UNQUOTE(JSON_EXTRACT(AI_PROMPT(template,value),'$.template'))), LENGTH(JSON_UNQUOTE(JSON_EXTRACT(AI_PROMPT(template,value),'$.args[0]'))) FROM prompt_lob")
            self.assertEqual(lengths, "80007\t80000")
            rendered = json.loads(self.sql("SELECT JSON_QUOTE(AI_COMPLETE('%s',AI_PROMPT(template,value))) FROM prompt_lob" % self.model))
            self.assertEqual(rendered, "t" * 80000 + "v" * 80000 + "/" + "v" * 80000)
        finally:
            self.sql("DROP TABLE prompt_lob")


if __name__ == "__main__":
    unittest.main(verbosity=2)
