import { createHash } from 'node:crypto';

// Use the complete Call-ID: sanitization or basename truncation merges callers.
export function callKey(callId) {
  return 'c_' + createHash('sha256').update(String(callId), 'utf8').digest('hex');
}
