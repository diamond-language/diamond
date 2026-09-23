# Package repository launch: next pre-1.0 release

The release highlight is the public package repository going live. Target the
next suitable minor release: **0.7**, or **0.8** if the work spans another release.
Choose the version when scheduling the release; do not bump it solely to reserve
this milestone. The current runtime remains 0.6.0.

## Release outcome

A user can discover the public registry, add a published cut with facet, commit
its lockfile, and reproduce the installation on a clean machine. Maintainers can
publish immutable releases and manage access. An operator can monitor, back up,
restore, and handle a compromised credential or package takedown.

The initial launch proposal is public reads with operator-provisioned publishers.
Self-service account registration, a large search UI, provenance automation, and
multi-node hosting can follow. Confirm the publisher policy and minimum browsing
experience before announcing availability.

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
   external probes, alert delivery, log retention, and off-host backup schedules.
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

A passing local drill establishes test evidence, not public deployment. DNS,
production credentials, the hosting choice, and announcement remain launch
inputs. The [operations checklist](../applications/registry/deploy/OPERATIONS.md)
and [registry implementation plan](package-registry-plan.md) track the details.

## Next work

After the QEMU proxy gate, prepare the reproducible launch inventory and its
staging publication rehearsal. Resolve public hostname and initial publisher
policy while that work proceeds.
