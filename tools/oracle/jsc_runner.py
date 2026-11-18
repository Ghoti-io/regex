#!/usr/bin/env python3
"""How this tree invokes JavaScriptCore, the ECMAScript second opinion.

V8 and SpiderMonkey are not two opinions (SpiderMonkey runs V8's irregexp),
so a behaviour of one is a behaviour of both. JavaScriptCore has its own
engine, which is what lets a differential say "V8 alone" rather than guess.
The pin and the reason are in containers/IMAGES; `jsc_match.js` is the
program that runs inside, speaking the protocol `node_match.mjs` speaks.

Unlike node_runner.py there is no flag to apply: the shell has no regular-
expression tier-up switch to pin, and the answers do not depend on how many
rows came first. What is asserted instead is the version, through
oracle_env.check_pin, so a drifted image stops the run.

Copyright 2026 by Corey Pennycuff
"""

import json
import os
import subprocess

import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "jsc_match.js")


def provenance():
    """The line a run prints to say which JavaScriptCore answered."""
    return "jsc: %s" % oracle_env.check_pin("jsc")


def ask(rows):
    """[flags, pattern, subject] rows to results, as node_match.mjs gives them.

    Each result is "syntax", "surrogate", None for no match, or the byte spans
    of every group.
    """
    oracle_env.check_pin("jsc")
    payload = json.dumps([[f, p, s] for f, p, s in rows]) + "\n"
    finished = subprocess.run(
        oracle_env.command("jsc", ["jsc", SCRIPT]),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)
