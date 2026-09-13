export const DEFAULT_SOFTMODEM_BINARY = '/opt/sipfax/bin/sipfax-softmodem';
export const DEFAULT_SLMODEM_BRIDGE = '/opt/sipfax/bin/sipfax-slmodem-bridge';
export const DEFAULT_LINMODEM_BINARY = '/opt/sipfax/bin/sipfax-linmodem';

// Resolve for each new call so persisted modem changes take effect on that call.
export function resolveModemCommand(modem, env = process.env) {
  if (modem.command !== null && modem.command !== undefined) return modem.command;
  if (modem.engine === 'linmodem') return env.SIPFAX_LINMODEM_BINARY ?? DEFAULT_LINMODEM_BINARY;
  if (modem.engine === 'slmodem') return env.SIPFAX_SLMODEM_BRIDGE ?? DEFAULT_SLMODEM_BRIDGE;
  return env.SIPFAX_SOFTMODEM_BINARY ?? DEFAULT_SOFTMODEM_BINARY;
}
