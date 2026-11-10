#!/bin/sh
# Exercise bootstrap recovery without touching the host package database.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/bin"
cat > "$tmp/bin/apt-get" <<'MOCK'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$STATE/args"
case " $* " in
    *' update '*) action=update ;;
    *' install '*) action=install ;;
    *) exit 99 ;;
esac
echo "$action" >> "$STATE/actions"
count=$(cat "$STATE/$action" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" > "$STATE/$action"
case "$SCENARIO:$action:$count" in
    update-fails:update:1|install-fails:install:1|always-fails:update:*) exit 100 ;;
esac
MOCK
printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/sleep"
chmod +x "$tmp/bin/apt-get" "$tmp/bin/sleep"
export PATH="$tmp/bin:$PATH"
for SCENARIO in success update-fails install-fails always-fails; do
    STATE="$tmp/$SCENARIO"
    export STATE SCENARIO
    mkdir "$STATE"
    status=0
    sh "$root/tools/ci_apt.sh" 'example-package=1.2' > "$STATE/log" 2>&1 || status=$?
    case "$SCENARIO" in
        success) expected='update install'; expected_status=0 ;;
        update-fails) expected='update update install'; expected_status=0 ;;
        install-fails) expected='update install update install'; expected_status=0 ;;
        always-fails) expected='update update update'; expected_status=1 ;;
    esac
    actual=$(tr '\n' ' ' < "$STATE/actions")
    [ "$actual" = "$expected " ] && [ "$status" -eq "$expected_status" ] || {
        echo "FAIL $SCENARIO: status=$status actions=$actual" >&2
        exit 1
    }
    grep -q 'APT::Update::Error-Mode=any update' "$STATE/args"
    grep -q 'Acquire::http::No-Cache=true' "$STATE/args"
    if [ "$status" -eq 0 ]; then
        grep -q 'install -y example-package=1.2' "$STATE/args"
    fi
done
echo 'APT bootstrap recovery checks passed'
