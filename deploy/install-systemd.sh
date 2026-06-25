#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${EUID}" -ne 0 ]]; then
  echo "install-systemd.sh must be run as root" >&2
  exit 1
fi

if ! id sipfax >/dev/null 2>&1; then
  useradd --system --home-dir /opt/sipfax --shell /usr/sbin/nologin sipfax
fi

install -d -m 0750 -o root -g sipfax /etc/sipfax
if [[ ! -f /etc/sipfax/sipfax.env ]]; then
  install -m 0640 -o root -g sipfax "${repo_root}/deploy/sipfax.env.example" /etc/sipfax/sipfax.env
  echo "Created /etc/sipfax/sipfax.env from template; edit placeholders before starting sipfax.service"
fi

install -m 0644 "${repo_root}/deploy/sipfax.service" /etc/systemd/system/sipfax.service
install -d -m 0755 -o sipfax -g sipfax /var/cache/sipfax
systemctl daemon-reload

echo "Installed sipfax.service. Run: systemctl enable --now sipfax.service"
