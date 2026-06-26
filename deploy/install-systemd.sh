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
if [[ ! -x "${repo_root}/vendor/sipfax-softmodem/sipfax-softmodem" ]]; then
  echo "Missing vendor/sipfax-softmodem/sipfax-softmodem; run 'make -C vendor/sipfax-softmodem' before installing" >&2
  exit 1
fi
install -d -m 0755 -o root -g sipfax /opt/sipfax/bin
install -m 0755 -o root -g sipfax "${repo_root}/vendor/sipfax-softmodem/sipfax-softmodem" /opt/sipfax/bin/sipfax-softmodem
install -d -m 0755 -o root -g root /usr/lib/sipfax
install -m 0755 -o root -g root "${repo_root}/bin/sipfax-egress-apply" /usr/lib/sipfax/sipfax-egress-apply
install -d -m 0755 -o root -g root /etc/ppp/ip-up.d /etc/ppp/ip-down.d
install -m 0755 -o root -g root "${repo_root}/deploy/ppp/ip-up" /etc/ppp/ip-up.d/sipfax
install -m 0755 -o root -g root "${repo_root}/deploy/ppp/ip-down" /etc/ppp/ip-down.d/sipfax
install -d -m 0755 -o sipfax -g sipfax /var/cache/sipfax
install -d -m 0755 -o sipfax -g sipfax /var/log/sipfax
systemctl daemon-reload

echo "Installed sipfax.service, softmodem worker, pppd hooks, and SIPfax egress helper. Run: systemctl enable --now sipfax.service"
