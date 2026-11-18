# JavaScriptCore, the second ECMAScript regular-expression engine.
#
# V8 and SpiderMonkey are not two opinions: SpiderMonkey runs V8's irregexp,
# imported, so a behaviour of that code answers twice and corroborates
# nothing. JavaScriptCore has its own regular-expression engine (YARR), which
# is what makes it a reference worth having beside node.
#
# **Built here**, because there is no official image: the shell (`jsc`) comes
# from Debian's WebKitGTK package. What is pinned is the base by digest, and
# the run-time version check in oracle_env.py is the guarantee, for the reason
# the posix image gives: an apt install is not pinned by the digest of what it
# fetches, so a security update moves it and the check is what notices.
#
# Nothing else is installed. The shell reads a script and standard input and
# has no business with the network, which `oracle_env.command` also closes.

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends libjavascriptcoregtk-bin \
 && rm -rf /var/lib/apt/lists/*

ENTRYPOINT []
CMD ["sh", "-c", "dpkg-query -W -f='JavaScriptCore (WebKitGTK ${Version})\\n' libjavascriptcoregtk-bin"]
