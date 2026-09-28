#!/usr/bin/env python3
# Fern-FT8, an FT8 decoder module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a .fernmod package against docs/MODULES.md in FernSDR.

Reads the package the way an installer would: the magic line, the manifest
length, the manifest, then exactly `size` bytes of executable whose SHA-256
must match. Exits with status 1 and a reason when anything is off.
"""

import argparse
import hashlib
import json
import math
import os
import re
import sys

MAGIC = b"FERNMOD1\n"
PLATFORMS = ("linux-x86_64", "linux-aarch64", "linux-armhf")
MAX_MANIFEST = 16 * 1024
MAX_EXECUTABLE = 32 * 1024 * 1024
ID_PATTERN = re.compile(r"[a-z][a-z0-9-]{0,31}\Z")
# One spelling per number: FernSDR refuses leading zeros.
VERSION_PATTERN = re.compile(r"(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\Z")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
KEY_PATTERN = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")
SETTING_TYPES = ("string", "number", "boolean", "choice")
BAND_KEYS = ("sample_rate", "center", "signal")
# The limits FernSDR applies when it installs a package.
MAX_SETTINGS = 64
MAX_CHOICES = 64
TEXT_LIMITS = {"name": 64, "license": 100, "source": 300, "description": 300}
SETTING_TEXT_LIMITS = {"label": 64, "help": 400, "unit": 16}


class Invalid(Exception):
    pass


def no_duplicates(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise Invalid("duplicate key %r in the manifest" % key)
        seen[key] = value
    return seen


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def utf8_length(text):
    return len(text.encode("utf-8"))


def check_default(key, kind, s, default):
    if kind == "string":
        if not isinstance(default, str) or utf8_length(default) > 1024:
            raise Invalid("setting %s: default is not a string of at most 1024 bytes" % key)
    elif kind == "number":
        if not number(default):
            raise Invalid("setting %s: default is not a number" % key)
        if ("min" in s and default < s["min"]) or ("max" in s and default > s["max"]):
            raise Invalid("setting %s: default is out of range" % key)
    elif kind == "boolean":
        if not isinstance(default, bool):
            raise Invalid("setting %s: default is not a boolean" % key)
    elif default not in s["choices"]:
        raise Invalid("setting %s: default %r is not a choice" % (key, default))


def check_settings(settings):
    if not isinstance(settings, list):
        raise Invalid("settings is not a list")
    if len(settings) > MAX_SETTINGS:
        raise Invalid("more than %d settings" % MAX_SETTINGS)
    keys = set()
    for s in settings:
        if not isinstance(s, dict):
            raise Invalid("a setting is not an object")
        key = s.get("key")
        if not isinstance(key, str) or not KEY_PATTERN.match(key) or key in BAND_KEYS or key in keys:
            raise Invalid("bad, reserved or repeated setting key %r" % key)
        keys.add(key)
        kind = s.get("type")
        if kind not in SETTING_TYPES:
            raise Invalid("setting %s has type %r" % (key, kind))
        for field, limit in SETTING_TEXT_LIMITS.items():
            if field in s and (not isinstance(s[field], str) or utf8_length(s[field]) > limit):
                raise Invalid("setting %s: %s is not a string of at most %d bytes" % (key, field, limit))
        if "live" in s and not isinstance(s["live"], bool):
            raise Invalid("setting %s: live is not a boolean" % key)
        for bound in ("min", "max"):
            if bound in s and not number(s[bound]):
                raise Invalid("setting %s: %s is not a number" % (key, bound))
        if "min" in s and "max" in s and s["min"] > s["max"]:
            raise Invalid("setting %s: min is above max" % key)
        if kind == "choice":
            choices = s.get("choices")
            if (not isinstance(choices, list) or not 1 <= len(choices) <= MAX_CHOICES or
                    not all(isinstance(c, str) and 1 <= utf8_length(c) <= 64 for c in choices)):
                raise Invalid("setting %s needs 1 to %d choices of 1 to 64 bytes" % (key, MAX_CHOICES))
        if s.get("default") is not None:
            check_default(key, kind, s, s["default"])


def check_manifest(manifest):
    if not isinstance(manifest, dict):
        raise Invalid("the manifest is not an object")
    if manifest.get("schema") != 1:
        raise Invalid("schema is %r" % manifest.get("schema"))
    module_id = manifest.get("id")
    if not isinstance(module_id, str) or not ID_PATTERN.match(module_id):
        raise Invalid("id %r is not valid" % module_id)
    version = manifest.get("version")
    if not isinstance(version, str) or not VERSION_PATTERN.match(version):
        raise Invalid("version %r is not major.minor.patch without leading zeros" % version)
    # Input modules speak API 1 and decoders API 2, as FernSDR checks.
    if (manifest.get("kind"), manifest.get("api")) not in (("input", 1), ("decoder", 2)):
        raise Invalid("kind %r, api %r" % (manifest.get("kind"), manifest.get("api")))
    platform = manifest.get("platform")
    if platform not in PLATFORMS:
        raise Invalid("platform %r is not valid" % platform)
    for field, limit in TEXT_LIMITS.items():
        value = manifest.get(field)
        if not isinstance(value, str) or not value or utf8_length(value) > limit:
            raise Invalid("%s is missing or longer than %d bytes" % (field, limit))
    size = manifest.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size < 1 or size > MAX_EXECUTABLE:
        raise Invalid("size %r is not valid" % size)
    sha256 = manifest.get("sha256")
    if not isinstance(sha256, str) or not SHA256_PATTERN.match(sha256):
        raise Invalid("sha256 %r is not valid" % sha256)
    check_settings(manifest.get("settings"))


def check(path, extract=None):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(MAGIC):
        raise Invalid("no FERNMOD1 line")
    pos = len(MAGIC)
    end = data.find(b"\n", pos, pos + 6)
    if end < 0:
        raise Invalid("no manifest length line of at most five digits")
    length_text = data[pos:end]
    if not length_text.isdigit() or (len(length_text) > 1 and length_text.startswith(b"0")):
        raise Invalid("manifest length %r is not a decimal number" % length_text)
    length = int(length_text)
    if length > MAX_MANIFEST:
        raise Invalid("manifest of %d bytes is larger than %d" % (length, MAX_MANIFEST))
    pos = end + 1
    if pos + length > len(data):
        raise Invalid("the file ends inside the manifest")
    try:
        manifest = json.loads(data[pos:pos + length].decode("utf-8"), object_pairs_hook=no_duplicates,
                              parse_constant=refuse_constant)
    except (UnicodeDecodeError, ValueError) as e:
        raise Invalid("the manifest is not UTF-8 JSON: %s" % e)
    try:
        # A \ud800 escape on its own decodes to a string no UTF-8 can hold.
        # FernSDR's parser refuses it, so a package that carries one must not
        # pass here either.
        json.dumps(manifest, ensure_ascii=False).encode("utf-8")
    except UnicodeEncodeError:
        raise Invalid("the manifest contains a lone UTF-16 surrogate, which is not Unicode text")
    check_manifest(manifest)
    executable = data[pos + length:]
    if len(executable) != manifest["size"]:
        raise Invalid("the executable has %d bytes, the manifest says %d" % (len(executable), manifest["size"]))
    if hashlib.sha256(executable).hexdigest() != manifest["sha256"]:
        raise Invalid("the SHA-256 of the executable does not match the manifest")

    module_id, version, platform = manifest["id"], manifest["version"], manifest["platform"]
    expected = "%s-%s-%s.fernmod" % (module_id, version, platform)
    if os.path.basename(path) != expected:
        raise Invalid("the file is named %s, but its manifest says %s" % (os.path.basename(path), expected))

    if extract:
        with open(extract, "wb") as f:
            f.write(executable)
        os.chmod(extract, 0o755)
    return manifest


def refuse_constant(name):
    # Python's json takes NaN and Infinity; FernSDR's parser, rightly, does not.
    raise Invalid("the manifest contains %s, which is not JSON" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package")
    parser.add_argument("--extract", help="write the executable to this file")
    parser.add_argument("--manifest", help="write the manifest to this file")
    args = parser.parse_args()
    try:
        manifest = check(args.package, args.extract)
    except (Invalid, OSError, ValueError) as e:
        # ValueError covers what slips past the checks, a lone surrogate that
        # cannot be encoded among them: refused with a reason, not a traceback.
        sys.exit("%s: %s" % (args.package, e))
    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, separators=(",", ":"))
    print("%s: %s %s for %s, %d bytes, %d settings" % (args.package, manifest["id"], manifest["version"],
                                                       manifest["platform"], manifest["size"],
                                                       len(manifest["settings"])))


if __name__ == "__main__":
    main()
