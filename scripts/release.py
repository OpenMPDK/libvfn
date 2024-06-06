#!/usr/bin/env python
# SPDX-License-Identifier: LGPL-2.1-or-later or MIT

import argparse
import sys
import re
from dataclasses import dataclass
from typing import Optional
import subprocess
import textwrap


def run(*args, **kwargs):
    kwargs = {
        "capture_output": True,
        "text": True,
        "encoding": "utf-8",
        "errors": "ignore",  # ignore encoding errors in output
        "check": True,
        **kwargs,
    }
    try:
        return subprocess.run(*args, **kwargs)
    except FileNotFoundError as e:
        print(f"error > binary '{e.filename}' not found")
    except subprocess.CalledProcessError as e:
        print(f"error > {' '.join(e.cmd)}  # return code: {e.returncode}")
        print("stdout:")
        print(e.stdout)
        print("stderr:")
        print(e.stderr)
        sys.exit(1)


@dataclass
class Version:
    major: int
    minor: int
    patch: int
    rc: Optional[int]

    @property
    def version(self):
        res = f"{self.major}.{self.minor}.{self.patch}"
        if self.rc:
            res += f"-rc{self.rc}"
        return res

    @property
    def tag(self):
        return f"v{self.version}"


def parse_version(arg):
    p = r"^v(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)(?:-rc(?P<rc>\d+))?$"
    match = re.match(p, arg)
    if not match:
        raise ValueError(f"invalid version number")
    return Version(
        major=int(match.group("major")),
        minor=int(match.group("minor")),
        patch=int(match.group("patch")),
        rc=int(match.group("rc")) if match.group("rc") else None,
    )


class CliParser(argparse.ArgumentParser):
    def error(self, message):
        sys.stderr.write(f"error: {message}\n")
        self.print_help()
        sys.exit(1)


def parse_cli():
    cli = CliParser(
        description="This script does all necessary steps to create a release, short of pushing upstream",
        epilog=textwrap.dedent("""

        Version string examples:
          release.sh v2.1.0-rc0     # v2.1.0 release candidate 0
          release.sh v2.1.0         # v2.1.0 release
        """),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    cli.add_argument(
        "version", type=parse_version, help="version tag of the new release"
    )
    cli.add_argument(
        "-d",
        "--no-doc",
        action="store_false",
        dest="build_docs",
        help="disable documentation building",
    )

    args = cli.parse_args()
    if args.version:
        return args
    else:
        cli.print_help()
        sys.exit(1)


def replace_first_match(fpath, pattern, replace_with):
    with open(fpath, "r") as fh:
        lines = fh.readlines()

    with open(fpath, "w") as fh:
        replaced = False
        for line in lines:
            if not replaced and re.search(pattern, line):
                replaced = True
                new = re.sub(pattern, replace_with, line)
                if new != line:
                    line = new
            fh.write(line)
        if replaced is False:
            raise Exception(f"No matches in {fpath}")


def main():
    args = parse_cli()
    ver = args.version

    res = run(["git", "status", "--porcelain"])
    if res.stdout:
        print("Git tree is dirty, commit or stash changes and try again")
        for line in res.stdout.split("\n"):
            print(line)
        sys.exit(1)

    res = run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    if res.stdout.strip() != "main":
        print("Not currently on 'main' branch, aborting!")
        sys.exit(1)

    replace_first_match(
        "meson.build",
        pattern=r"(\s*version\s*:\s*')([^']*)(')",
        replace_with=lambda m: f"{m.group(1)}{ver.version}{m.group(3)}",
    )

    replace_first_match(
        "flake.nix",
        pattern=r"""(\s*libvfnVersion\s*=\s*\")([^\"]+)";""",
        replace_with=lambda m: f'{m.group(1)}{ver.version}";',
    )

    run(["git", "add", "meson.build", "flake.nix"])
    run(["git", "commit", "-s", "-m", f"build: version {ver.tag}"])
    run(["git", "tag", "-s", "-m", f"release {ver.tag}", ver.tag])

    print("\nAll done!")
    print(f'  to push: git push <remote> "{ver.tag}":^{{}}:main tag "{ver.tag}"')


if __name__ == "__main__":
    main()
