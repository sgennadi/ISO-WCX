#!/usr/bin/env bash
# Hosted Ubuntu runners can include an unrelated Microsoft APT feed that
# intermittently answers 403. Our test packages all come from Ubuntu; disable
# only that extra source so a transient vendor outage cannot block release CI.
set -euo pipefail
shopt -s nullglob
for feed in /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
  if grep -qF 'packages.microsoft.com' "$feed"; then
    echo "::notice::Disabling unrelated Microsoft APT feed: ${feed##*/}"
    sudo mv -- "$feed" "$feed.iso-wcx-disabled"
  fi
done
sudo apt-get update -qq
