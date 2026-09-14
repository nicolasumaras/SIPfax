#!/bin/sh
# Fixed command only: never interpolate caller-controlled dialplan values.
# The dialplan holds the shared ATA call lock throughout the project call.
# Refuse activation if another ATA channel exists or any ATA call is Up.
channels=$(/usr/sbin/asterisk -rx 'core show channels concise') || exit 1
printf '%s\n' "$channels" | /usr/bin/awk -F '!' '
  $1 ~ /^PJSIP\/(23416874|63416874)-/ { n++; if ($5 == "Up") active=1 }
  END { exit (n > 1 || active) ? 1 : 0 }
' || exit 1
exec /usr/bin/timeout 22 /usr/bin/ssh -T -F /dev/null \
  -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=yes \
  -o IdentitiesOnly=yes \
  -o UserKnownHostsFile=/var/lib/asterisk/.ssh/sipfax_ata_known_hosts \
  -i /var/lib/asterisk/.ssh/sipfax_ata_prepare sipfax-ata@192.168.1.25
