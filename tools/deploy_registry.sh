#!/usr/bin/env bash
# Upgrade the live cuts registry to this checkout's HEAD: check whether an upgrade
# is needed, take a verified backup, build the bundle in the local build VM, upload
# it, run deploy/upgrade.sh as root, verify the public registry, and prune.
#
#   tools/deploy_registry.sh [--force] [--dry-run] [--no-verify]
#
# By default it stops when tools/registry_needs_upgrade.py says NONE or OPTIONAL;
# --force upgrades anyway. --dry-run prints every mutating command instead of
# running it (the read-only upgrade check still runs).
#
# Backups and bundles go under ~/diamond-registry-backups (override with
# REGISTRY_BACKUP_DIR), never inside a source checkout, and only the newest of each
# is kept. Needs: the build VM running (REGISTRY_VM_PORT, REGISTRY_VM_KNOWN_HOSTS as
# for tools/build_registry_vm.sh) and root ssh to the host (REGISTRY_HOST).
set -euo pipefail
force=false dry=false verify=true
for arg in "$@"; do
    case "$arg" in
        --force) force=true ;;
        --dry-run) dry=true ;;
        --no-verify) verify=false ;;
        *) echo "usage: $0 [--force] [--dry-run] [--no-verify]" >&2; exit 64 ;;
    esac
done
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
host=${REGISTRY_HOST:-root@dilang.tech}
backups=${REGISTRY_BACKUP_DIR:-$HOME/diamond-registry-backups}
vm_port=${REGISTRY_VM_PORT:-2222}
known_hosts=${REGISTRY_VM_KNOWN_HOSTS:-$repo/../.vm-build/known_hosts}
case "$backups" in "$repo"/*) echo "backup directory must be outside the source tree" >&2; exit 64 ;; esac

run() { if $dry; then echo "+ $*"; else "$@"; fi; }
vm() {
    ssh -F /dev/null -p "$vm_port" -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=yes \
        -o "UserKnownHostsFile=$known_hosts" builder@127.0.0.1 "$@"
}

git diff --quiet && git diff --cached --quiet || { echo "commit or stash tracked changes first" >&2; exit 1; }
git fetch -q origin main
git merge-base --is-ancestor HEAD origin/main || { echo "HEAD is not on origin/main; deploy merged code only" >&2; exit 1; }
revision=$(git rev-parse HEAD); short=${revision:0:8}

echo "== is an upgrade needed?"
verdict=0
python3 tools/registry_needs_upgrade.py --host "$host" || verdict=$?
if [[ $verdict -ne 20 ]] && ! $force; then
    echo; echo "Not upgrading (verdict $verdict). Re-run with --force to upgrade anyway."
    exit 0
fi

echo; echo "== build VM: make sure there is swap (vm.c needs more than the guest's RAM)"
if $dry; then echo "+ (in the VM) swapon /swapfile, creating a 4G swapfile first if missing"; else
    vm 'swapon --show | grep -q . || { [ -f /swapfile ] || { sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile >/dev/null; }; sudo swapon /swapfile; }; free -m | sed -n 1,3p'
fi

echo; echo "== verified backup of the live registry"
backup="$backups/cutbackup-$(date +%F)-pre-$short"
run mkdir -p -m 700 "$backups"
run python3 tools/fetch_registry_backup.py "$backup" --host "$host" \
    --app /opt/diamond-registry/current/app --data /var/lib/diamond-registry

echo; echo "== build the bundle at $short"
bundle="$backups/registry-bundle-${revision:0:12}"
run tools/build_registry_vm.sh "$bundle"

echo; echo "== upload and upgrade"
sha=$(cut -d' ' -f1 "$bundle/SHA256SUMS" 2>/dev/null || echo "<sha256>")
run scp -q "$bundle/registry-deployment.tar.gz" "$host:/root/registry-deployment.tar.gz"
run scp -q applications/registry/deploy/upgrade.sh "$host:/root/upgrade.sh"
run ssh "$host" "bash /root/upgrade.sh /root/registry-deployment.tar.gz $sha"

echo; echo "== verify the public registry"
if ! $dry; then
    [[ -x build/facet ]] || make -s facet >/dev/null
    curl -fsS https://cuts.dilang.tech/health; echo
fi
if $verify; then run python3 tools/verify_registry_launch.py https://cuts.dilang.tech
else echo "skipped: --no-verify (run tools/verify_registry_launch.py after publishing the new versions)"; fi

echo; echo "== prune: keep only the newest backup and bundle, remove uploads from the host"
run ssh "$host" 'rm -f /root/registry-deployment.tar.gz /root/upgrade.sh'
if ! $dry; then
    for kind in cutbackup registry-bundle; do
        newest=$(ls -1dt "$backups"/$kind-* | head -n1)
        for old in "$backups"/$kind-*; do [[ "$old" == "$newest" ]] || rm -rf -- "$old"; done
    done
fi
if $dry; then echo; echo "Dry run: nothing was changed."; else
    echo; echo "Upgraded to $revision. Update the 'Runtime revision' line in applications/registry/deploy/PRODUCTION.md."
fi
