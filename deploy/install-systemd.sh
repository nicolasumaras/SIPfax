#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

engine=spandsp
check_only=false
for argument in "$@"; do
  case "$argument" in
    --engine=linmodem) engine=linmodem ;;
    --engine=spandsp) engine=spandsp ;;
    --check) check_only=true ;;
    *) echo "Usage: $0 [--engine=linmodem|--engine=spandsp] [--check]" >&2; exit 64 ;;
  esac
done
worker=vendor/sipfax-softmodem/sipfax-softmodem
if [[ "$engine" == linmodem ]]; then worker=vendor/linmodem/lm; fi
if [[ ! -x "${repo_root}/${worker}" ]]; then
  echo "Missing executable ${worker}; build the selected modem before installing" >&2
  exit 1
fi
for asset in package.json package-lock.json public/admin.html src/index.js bin/sipfax-call-key.mjs bin/sipfax-egress-apply bin/sipfax-linmodem; do
  [[ -f "${repo_root}/${asset}" ]] || { echo "Missing ${asset}" >&2; exit 1; }
done
# ELF workers must be loadable on the deployment host, not just executable.
# ldd can return success while reporting an unavailable symbol version.
if [[ "$(head -c 4 "${repo_root}/${worker}")" == $'\177ELF' ]]; then
  if ! dependencies=$(LC_ALL=C ldd "${repo_root}/${worker}" 2>&1) ||
     [[ "$dependencies" == *"not found"* ]]; then
    echo "Cannot load ${worker} on this host: ${dependencies}" >&2
    exit 1
  fi
fi
if [[ "$check_only" == true ]]; then
  echo "Install preflight passed: ${engine} (${worker})"
  exit 0
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "install-systemd.sh must be run as root" >&2
  exit 1
fi

if systemctl is-active --quiet sipfax.service; then
  echo "Stop SIPfax and wait for its PPP teardown hooks before installing" >&2
  exit 1
fi

if ! id sipfax >/dev/null 2>&1; then
  useradd --system --home-dir /opt/sipfax --shell /usr/sbin/nologin sipfax
fi

if [[ ! -d /etc/sipfax ]]; then
  install -d -m 0750 -o root -g sipfax /etc/sipfax
fi
if [[ ! -f /etc/sipfax/sipfax.env ]]; then
  install -m 0640 -o root -g sipfax "${repo_root}/deploy/sipfax.env.example" /etc/sipfax/sipfax.env
  if [[ "$engine" == linmodem ]]; then
    sed -i 's/^SIPFAX_MODEM_ENGINE=.*/SIPFAX_MODEM_ENGINE=linmodem/; s/^SIPFAX_MAX_SESSIONS=.*/SIPFAX_MAX_SESSIONS=1/' /etc/sipfax/sipfax.env
  fi
  echo "Created /etc/sipfax/sipfax.env from template; edit placeholders before starting sipfax.service"
fi

install -m 0644 "${repo_root}/deploy/sipfax.service" /etc/systemd/system/sipfax.service
install -d -m 0755 -o root -g sipfax /opt/sipfax/bin
if [[ "$repo_root" != /opt/sipfax ]]; then
  install -d -m 0755 -o root -g sipfax /opt/sipfax/src /opt/sipfax/public
  install -m 0644 -o root -g sipfax "${repo_root}/src/"*.js /opt/sipfax/src/
  install -m 0644 -o root -g sipfax "${repo_root}/public/admin.html" /opt/sipfax/public/admin.html
  install -m 0644 -o root -g sipfax "${repo_root}/package.json" "${repo_root}/package-lock.json" /opt/sipfax/
  install -m 0644 -o root -g sipfax "${repo_root}/bin/sipfax-call-key.mjs" /opt/sipfax/bin/sipfax-call-key.mjs
fi
if [[ "$engine" == linmodem ]]; then
  install -d -m 0755 -o root -g sipfax /opt/sipfax/vendor/linmodem
  if [[ "$repo_root" != /opt/sipfax ]]; then
    install -m 0755 -o root -g sipfax "${repo_root}/vendor/linmodem/lm" /opt/sipfax/vendor/linmodem/lm
    install -m 0755 -o root -g sipfax "${repo_root}/bin/sipfax-linmodem" /opt/sipfax/bin/sipfax-linmodem
  fi
else
  install -m 0755 -o root -g sipfax "${repo_root}/vendor/sipfax-softmodem/sipfax-softmodem" /opt/sipfax/bin/sipfax-softmodem
fi
install -d -m 0755 -o root -g root /usr/lib/sipfax
install -m 0755 -o root -g root "${repo_root}/bin/sipfax-egress-apply" /usr/lib/sipfax/sipfax-egress-apply
install -m 0644 -o root -g root "${repo_root}/bin/sipfax-call-key.mjs" /usr/lib/sipfax/sipfax-call-key.mjs
install -d -m 0755 -o root -g root /etc/ppp/ip-up.d /etc/ppp/ip-down.d
install -m 0755 -o root -g root "${repo_root}/deploy/ppp/ip-up" /etc/ppp/ip-up.d/sipfax
install -m 0755 -o root -g root "${repo_root}/deploy/ppp/ip-down" /etc/ppp/ip-down.d/sipfax
install -d -m 0755 -o sipfax -g sipfax /var/cache/sipfax
install -d -m 0755 -o sipfax -g sipfax /var/log/sipfax
systemctl daemon-reload

echo "Installed sipfax.service, ${engine} worker, application, pppd hooks, and SIPfax egress helper. Complete PPP credential migration/drop-in setup before starting. Existing saved configuration is preserved."
