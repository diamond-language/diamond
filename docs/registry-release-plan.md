# Package repository launch: Diamond 0.7.0

The selected release is **0.7.0**, highlighting the live public package registry.
Release checks passed on `b98f27cc`; the `v0.7.0` tag marks the commit that
records them, which differs only in this document.

## Release outcome

A user can discover the public registry, add a published cut with facet, commit
its lockfile, and reproduce the installation on a clean machine. Maintainers can
publish immutable releases and manage access. An operator can monitor, back up,
restore, and handle a compromised credential or package takedown.

The confirmed launch policy is public reads with operator-approved maintainers
and scoped publishing credentials.
Self-service account registration, a large search UI, provenance automation, and
multi-node hosting can follow. Review the minimum browsing experience before announcing availability.

## Gates and order

1. **QEMU staging.** Use a local Ubuntu guest with NAT and loopback-only SSH
   forwarding. Build from the source snapshot in a separate temporary directory.
   Run the actual nginx template through syntax validation, HTTPS facet
   publish/install, upload rejection, read/write throttling, and recovery. Also
   run the existing authentication, administration, monitoring, and backup/restore
   integration suite. `tools/test_registry_vm.sh` is the repeatable gate.
2. **Release inventory.** Select bundled cuts for launch, freeze their individual
   versions and dependency ranges, build and verify their archives, and record
   digests. Determine dependency publication order and rehearse the complete seed
   in staging. Keep credentials and private keys outside the inventory.
3. **Public endpoint and operations.** Select the hostname, host, service owner,
   publisher policy, and incident contact. Configure TLS, network restrictions,
   external probes, alert delivery, log retention, and the chosen manual off-host
   backup procedure.
   Demonstrate restoration and record recovery targets. Validate the systemd
   deployment on the target host; the temporary nginx drill does not cover it.
4. **User path.** Provide a landing page or catalog and exact getting-started,
   publishing, version selection, and locked reinstall instructions. Use the
   final public URL in examples only after it is selected. Confirm clean-machine
   installation, including transitive dependencies, against the launch seed.
5. **Launch and release.** Run the release test gates, review the archive inventory,
   publish the selected seed, and verify public HTTPS installs from outside the
   host. Only then finalize the Diamond minor version, changelog, release notes,
   tag, and announcement. Record the deployed revision and rollback snapshot.

Public deployment is complete. Local drill evidence and production verification
are recorded separately; remaining operational choices and the announcement
are tracked below. The [operations checklist](../applications/registry/deploy/OPERATIONS.md)
and [registry implementation plan](package-registry-plan.md) track the details.

## Next work

The public registry and catalog are live at `https://cuts.dilang.tech` on the
existing droplet. See the [production record](../applications/registry/deploy/PRODUCTION.md)
for the installed revision, service layout, verification, and operational limits.
The first manual laptop snapshot has passed restore verification. Automatic
alerts are deferred by the operator. Diamond 0.7.0 is tagged. The production reboot drill passed on 2026-09-24; the
first certificate renewal (~2026-10-30) remains to be confirmed.

## Public launch inventory

The public selection contains **18** bundled cuts, including `registry`.
The operator excluded `dials`, `active_auth`, `active_discussion`, `active_karma`,
`active_social`, and `active_tagging`; these were unpublished and removed from
the seed selection. Their source packages remain available in this repository. The explicit
selection and versions are in
[`applications/registry/launch-cuts.json`](../applications/registry/launch-cuts.json).
[`registry-launch-inventory.json`](registry-launch-inventory.json) records each
verified archive's name, version, dependency ranges, SHA-256, byte size, and file
name, in deterministic dependency-first publication order. Changing a selected package requires regenerating and reviewing
its inventory entry before the launch gate will pass.

After 0.7.0, every selected cut received a patch release that adds
`maintainers`; the checked-in selection and inventory describe those versions.
Production still serves the launch versions until an operator publishes the
new seed. Until then, `verify_registry_launch.py` against production fails on
the missing versions; the launch digests remain in the `v0.7.0` tag.

Build the exact candidate without publishing anything:

```sh
make facet
python3 tools/prepare_registry_seed.py /tmp/registry-launch-seed \
  --expect docs/registry-launch-inventory.json
```

The destination must not exist. Archives and `inventory.json` are written there;
failed preparation removes only the newly created destination. The builder uses
facet's archive verifier, checks selected identities, rejects missing dependencies
and cycles, and compares the result to the reviewed inventory. Dependency range
compatibility is checked by facet resolution during the staging rehearsal. No
registry URL, credential, timestamp, or machine-specific path enters the inventory.

To propose a changed candidate, use a fresh destination without `--expect`, inspect
the archives and the inventory diff, and update the checked-in inventory only after
review. Package versions are independent of Diamond's eventual 0.7/0.8 version.
Before publishing a previously released package, increment its cut version if its
bytes changed: registry releases cannot be replaced.

`tools/test_registry_vm.sh` now includes `make test-registry-seed`. The rehearsal
rebuilds the reviewed archives in QEMU, publishes those exact bytes through the real
nginx HTTPS proxy in dependency order, and respects its six-writes-per-minute
policy. It compares returned archive identities, installs the graph through facet
using only root cuts as direct dependencies, loads every cut, and reinstalls from
the unchanged lockfile. All data and credentials are temporary. This is a local
rehearsal; final public publication remains a separate release action.

The 2026-09-23 QEMU rehearsal passed for the original candidate: all 24 archives matched
the host-built inventory, published successfully, resolved through facet, loaded,
and reinstalled from an unchanged lockfile. The public registry was subsequently
activated, seeded, and reduced to the approved 18 cuts. The current selection
retains the reviewed bytes and has no dependency on the six removed cuts.

## Release checklist

Release notes are in [registry-release-notes.md](registry-release-notes.md).
The selected version is 0.7.0.

- [x] Deploy HTTPS catalog and registry; preserve existing host routes.
- [x] Publish and verify the final 18-cut selection; revoke temporary credentials.
- [x] Verify removed cuts are unavailable and retained dependency graphs resolve.
- [x] Record the deployed runtime revision and configuration backup procedure.
- [x] Download and restore-verify the first snapshot with
  `tools/fetch_registry_backup.py`. Saved under `~/Projects/diamond-lang/cutbackup`;
  see the production record for its verification timestamp and retained state.
- **Deferred by operator:** automatic alerts and receiver selection. Manual
  `monitor.py` probes remain available; automatic notification is not a release gate.
- **Deferred past 0.7.0, since resolved in part:** the production reboot drill
  passed on 2026-09-24. The first certificate renewal (modartist.app, ~2026-10-30)
  has not yet happened; confirm it afterwards.
- [x] Choose 0.7.0 and finalize the release notes.
- [x] Update `DIAMOND_VERSION` in `src/main.c` and move the selected Unreleased
  changelog entries under the version/date heading.
- [x] Verify the final release commit: inspect the complete CI results, run the
  QEMU registry gate (`tools/test_registry_vm.sh`), and run
  `python3 tools/verify_registry_launch.py https://cuts.dilang.tech`.
  Record the tested SHA and outcomes; earlier runs do not certify a later commit.
  Verified 2026-09-23 on `b98f27cc`: all nine CI jobs passed (run 35922577277);
  `tools/test_registry_vm.sh all` passed (seed, nginx proxy, HTTP, Caddy, systemd
  lifecycle and restore); the public verifier resolved, digest-checked, loaded,
  and reinstalled all 18 cuts. The preceding commit's Ubuntu failure was a missing
  `curl` in the CI image, fixed in `b98f27cc`.
- [x] Confirm the version output, clean working tree, and pushed release commit;
  create the version tag. The release is the tag plus the in-repo release notes;
  no GitHub Release or binaries are published yet.

The backup helper requires a new directory and verifies the downloaded snapshot
by restoring it into a temporary local directory. Choose its destination outside
this source checkout. Follow the [production backup procedure](../applications/registry/deploy/PRODUCTION.md#current-backup-and-alert-choices).
