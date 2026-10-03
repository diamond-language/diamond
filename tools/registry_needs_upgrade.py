#!/usr/bin/env python3
"""Say whether the live registry needs upgrading to a given revision.

The registry ships as one bundle built from a single repo revision, but most
releases change nothing it runs. This compares the revision currently deployed
(read from the server, or given with --deployed) against a target revision
(default HEAD) and classifies what changed:

  REQUIRED  something the registry itself executes changed: its app files, a cut
            it loads (the dependency closure of its `require_cut` lines), or
            the facet it publishes/installs with.  exit 20
  OPTIONAL  only the Diamond runtime changed (src/, lib/, reginold/, Makefile).
            The registry keeps working on its current interpreter; upgrade for a
            crash or security fix that affects it, otherwise wait for the next
            REQUIRED change.  exit 10
  NONE      nothing relevant changed.  exit 0

Changes to the bundled seed (docs/ and launch-cuts.json) and to deploy/ templates
never require an upgrade: new cut versions are published through the API, and
deploy/ files are reference configs applied by hand.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = "root@dilang.tech"
RUNTIME_PATHS = ["src/", "lib/", "reginold/", "Makefile"]
FACET_PATHS = ["tools/facet.c", "tools/semver.c", "tools/semver.h"]


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True,
                          capture_output=True, text=True).stdout


def shipped_app_files():
    """The registry app files the bundle copies, read from the build script so
    the two cannot drift. Falls back to the whole directory if that fails."""
    script = (ROOT / "tools/build_registry_vm.sh").read_text()
    match = re.search(r"cp applications/registry/\{([^}]+)\} payload/app/", script)
    if not match:
        return ["applications/registry/"]
    return ["applications/registry/" + name for name in match.group(1).split(",")]


def cut_closure(roots):
    """Every cut the registry loads: its require_cut roots plus their
    dependencies, read from each packages/<name>/diamond.cut."""
    seen, queue = set(), list(roots)
    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)
        manifest = ROOT / "packages" / name / "diamond.cut"
        if manifest.exists():
            queue.extend(json.loads(manifest.read_text()).get("dependencies", {}))
    return sorted(seen)


def registry_roots():
    names = set()
    for source in (ROOT / "applications/registry").glob("*.di"):
        names.update(re.findall(r'require_cut\s+"([a-z_0-9]+)"', source.read_text()))
    return sorted(names)


def deployed_revision(host):
    target = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host,
         "readlink /opt/diamond-registry/current"],
        capture_output=True, text=True)
    if target.returncode != 0:
        sys.exit(f"cannot read the deployed revision from {host}: {target.stderr.strip()}")
    revision = Path(target.stdout.strip()).name
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        sys.exit(f"unexpected deployed revision {revision!r}")
    return revision


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--deployed", help="deployed revision (default: read from --host)")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--target", default="HEAD")
    args = parser.parse_args()

    deployed = args.deployed or deployed_revision(args.host)
    try:
        git("cat-file", "-e", deployed + "^{commit}")
    except subprocess.CalledProcessError:
        sys.exit(f"deployed revision {deployed[:12]} is not in this checkout (git fetch?)")
    target = git("rev-parse", args.target).strip()
    changed = git("diff", "--name-only", deployed, target).splitlines()

    cuts = cut_closure(registry_roots())
    required_prefixes = (shipped_app_files() + FACET_PATHS +
                         [f"packages/{name}/" for name in cuts])

    # Docs and tests inside a package are not executed by the registry.
    inert = re.compile(r"^packages/[^/]+/(README\.md|LICENSE|test\.sh|test/)")

    def hits(prefixes):
        return [path for path in changed if not inert.match(path)
                and any(path == p or (p.endswith("/") and path.startswith(p)) for p in prefixes)]

    required = hits(required_prefixes)
    runtime = hits(RUNTIME_PATHS)
    print(f"deployed {deployed[:12]} -> target {target[:12]}: {len(changed)} files changed")
    print(f"registry loads cuts: {', '.join(cuts)}")
    if required:
        print("\nREQUIRED -- the registry's own code changed:")
        for path in required:
            print(f"  {path}")
        if runtime:
            print(f"(plus {len(runtime)} runtime file(s))")
        return 20
    if runtime:
        print(f"\nOPTIONAL -- only the Diamond runtime changed ({len(runtime)} file(s)):")
        for path in runtime[:15]:
            print(f"  {path}")
        subjects = git("log", "--no-merges", "--format=  %h %s", f"{deployed}..{target}",
                       "--", *RUNTIME_PATHS).splitlines()
        print("runtime commits since deploy:")
        print("\n".join(subjects[:15]))
        print("\nUpgrade only for a crash/security fix that affects the registry; otherwise "
              "wait for the next REQUIRED change.")
        return 10
    print("\nNONE -- nothing the registry runs has changed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
