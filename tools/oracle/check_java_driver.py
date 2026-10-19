#!/usr/bin/env python3
"""Controls that prove the Java line protocol and comparator can fail."""
import json
import os
import re
import subprocess
import sys

import java_probe
import oracle_env


def expect(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    for pin in ("java25", "java27"):
        oracle_env.check_pin(pin)
        controls = [row for row in java_probe.CASES if row["area"] == "control"]
        answers = java_probe.run(pin, controls)
        expect(answers[0]["kind"] == "match", "positive control")
        expect(answers[1]["kind"] == "nomatch", "negative control")
        expect(answers[2]["kind"] == "syntax", "syntax control")
        expect(answers[3]["groups"][1] is None, "unset capture control")
        expect(answers[4]["groups"][0] == [0, 4], "supplementary offset control")
        expect(answers[5]["groups"][0] == [4, 5], "find-from offset control")
        expect(answers[6]["kind"] == "compile", "compile verdict control")
        expect(answers[7]["kind"] == "syntax" and answers[7]["index"] == 4,
               "multibyte syntax offset control")
        template = java_probe.run(pin, [java_probe.case("control", "a", "a",
                                 mode="replace-all", argument="$")])[0]
        expect(template["kind"] == "template" and template["driver_error"],
               "malformed replacement template control")
        driver = oracle_env.command(pin, ["java", "tools/oracle/JavaOracle.java"])
        bad = subprocess.run(driver, input=("first\tz\t\t\t\n"
                 "first\tff\t\t\t\nfirst\t61\t\t61\t\n"
                 "bogus\t28\t\t\t\n"),
                             capture_output=True, text=True, timeout=30)
        expect(bad.returncode == 0, "malformed request killed batch")
        parsed = []
        for line in bad.stdout.splitlines():
            state, hex_data = line.split("\t")
            parsed.append((state, json.loads(bytes.fromhex(hex_data))))
        expect(len(parsed) == 4 and all(x[0] == "error" for x in parsed[:2]),
               "malformed input was not explicit")
        expect(all(x[1]["kind"] == "input" for x in parsed[:2]),
               "malformed input type")
        expect(parsed[2][0] == "ok" and parsed[2][1]["kind"] == "match",
               "driver did not recover after malformed input")
        expect(parsed[3][0] == "error" and parsed[3][1]["kind"] == "input"
               and parsed[3][1]["message"] == "unknown mode bogus",
               "unknown mode was hidden by syntax error")
        wrong = subprocess.run([sys.executable, "tools/oracle/java_probe.py",
                                "--pin", pin, "--wrong-expected-control"],
                               capture_output=True, text=True, timeout=180)
        summary = re.fullmatch(r"\d+ cases, \d+ agree, (\d+) differ",
                               wrong.stdout.splitlines()[-1]) if wrong.stdout else None
        expect(wrong.returncode != 0 and summary is not None
               and int(summary.group(1)) == 1,
               "planted wrong expectation passed")
    # A pin mapped to the other image must be rejected by its runtime probe.
    table = oracle_env.pins()
    old_pin = table["java25"]
    try:
        table["java25"] = (table["java27"][0], old_pin[1], old_pin[2])
        oracle_env._cache.pop(("version", "java25"), None)
        try:
            oracle_env.check_pin("java25")
        except oracle_env.OracleUnavailable:
            pass
        else:
            raise RuntimeError("wrong image passed runtime pin")
    finally:
        table["java25"] = old_pin
        oracle_env._cache.pop(("version", "java25"), None)
    old_pull = os.environ.get("GHOTI_ORACLE_PULL")
    try:
        table["java25"] = ("localhost/absent-java-oracle:never", old_pin[1], old_pin[2])
        os.environ["GHOTI_ORACLE_PULL"] = "0"
        try:
            oracle_env.ensure("java25")
        except oracle_env.OracleUnavailable:
            pass
        else:
            raise RuntimeError("missing image passed availability check")
    finally:
        table["java25"] = old_pin
        if old_pull is None:
            os.environ.pop("GHOTI_ORACLE_PULL", None)
        else:
            os.environ["GHOTI_ORACLE_PULL"] = old_pull
    for override in ({"GHOTI_ORACLE_MODE": "host"},
                     {"GHOTI_ORACLE_ALIAS": "java25=java27"}):
        environment = os.environ.copy()
        environment.update(override)
        attempt = subprocess.run([sys.executable, "tools/oracle/java_probe.py",
                                  "--pin", "java25", "--record"],
                                 capture_output=True, text=True,
                                 env=environment, timeout=10)
        expect(attempt.returncode != 0 and "recording requires pinned"
               in attempt.stderr, "unpinned recording was allowed")
    print("Java driver controls: match, no match, syntax, unset, supplementary, "
          "malformed input, template error, wrong expectation, wrong runtime, "
          "missing image, unpinned recording verified")


if __name__ == "__main__":
    sys.exit(main())
