#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a .fernmod package as docs/MODULES.md in FernSDR describes it.

The identity and the settings list in the manifest come from running
--describe on a build of the module that runs on this machine, so that the
packages of every platform carry the same list.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_fernmod import (ID_PATTERN, MAX_EXECUTABLE, MAX_MANIFEST, PLATFORMS,  # noqa: E402
                           VERSION_PATTERN, Invalid, check_manifest)


def fail(message):
    sys.exit("mkfernmod: " + message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, help="the module executable to package")
    parser.add_argument("--describe-with", required=True,
                        help="a build of the module that runs here; its --describe output supplies the manifest")
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--version", required=True, help="must equal the version --describe reports")
    parser.add_argument("--license", default="GPL-2.0-or-later")
    parser.add_argument("--source", default="https://github.com/Steven9101/Fern-FT8")
    parser.add_argument("--description", default="Decodes FT8 on the frequencies you choose, with WSJT-X's sensitivity")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    try:
        run = subprocess.run([args.describe_with, "--describe"], stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, timeout=10, check=True)
        describe = json.loads(run.stdout.decode("utf-8"))
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        fail("running %s --describe failed: %s" % (args.describe_with, e))

    module_id = describe.get("id")
    if not isinstance(module_id, str) or not ID_PATTERN.match(module_id):
        fail("id %r is not 1 to 32 lowercase letters, digits and dashes starting with a letter" % module_id)
    if not VERSION_PATTERN.match(args.version):
        fail("version %r is not major.minor.patch, each at most four digits without leading zeros" % args.version)
    if describe.get("version") != args.version:
        fail("--describe reports version %r, not %r" % (describe.get("version"), args.version))
    if describe.get("kind") != "decoder" or describe.get("api") != 2:
        fail("--describe reports kind %r and api %r" % (describe.get("kind"), describe.get("api")))
    if not isinstance(describe.get("settings"), list):
        fail("--describe has no settings list")

    with open(args.executable, "rb") as f:
        executable = f.read()
    if len(executable) > MAX_EXECUTABLE:
        fail("the executable has %d bytes, more than %d" % (len(executable), MAX_EXECUTABLE))

    manifest = {
        "schema": 1,
        "id": module_id,
        "name": describe["name"],
        "version": args.version,
        "kind": "decoder",
        "api": 2,
        "platform": args.platform,
        "size": len(executable),
        "sha256": hashlib.sha256(executable).hexdigest(),
        "license": args.license,
        "source": args.source,
        "description": args.description,
        "settings": describe["settings"],
    }
    try:
        check_manifest(manifest)
    except Invalid as e:
        fail("FernSDR would refuse this manifest: %s" % e)
    text = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(text) > MAX_MANIFEST:
        fail("the manifest has %d bytes, more than %d" % (len(text), MAX_MANIFEST))

    expected = "%s-%s-%s.fernmod" % (module_id, args.version, args.platform)
    if os.path.basename(args.output) != expected:
        fail("a package for this manifest must be named %s" % expected)

    temporary = args.output + ".tmp"
    with open(temporary, "wb") as f:
        f.write(b"FERNMOD1\n")
        f.write(b"%d\n" % len(text))
        f.write(text)
        f.write(executable)
    os.replace(temporary, args.output)
    print("%s: %d bytes, executable %d bytes, sha256 %s" %
          (args.output, os.path.getsize(args.output), len(executable), manifest["sha256"]))


if __name__ == "__main__":
    main()
