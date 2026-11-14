/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Probe serial formatting (pure C, no ESP-IDF) so it can be unit-tested on the
 * host.  See birdiebot_probe_identity.h.
 */

#include "birdiebot_probe_identity.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

size_t birdiebot_probe_format_serial(const uint8_t mac[6], char *out,
                                     size_t capacity)
{
  static const char hex[] = "0123456789ABCDEF";
  static const char prefix[] = BIRDIEBOT_PROBE_SERIAL_PREFIX;
  const size_t prefix_len = sizeof(prefix) - 1U; /* without NUL */
  size_t pos = 0U;

  if ((mac == NULL) || (out == NULL)) {
    return 0U;
  }
  if (capacity < BIRDIEBOT_PROBE_SERIAL_CAPACITY) {
    return 0U;
  }

  memcpy(out, prefix, prefix_len);
  pos = prefix_len;
  for (size_t i = 0U; i < 6U; i++) {
    out[pos++] = hex[(mac[i] >> 4) & 0x0FU];
    out[pos++] = hex[mac[i] & 0x0FU];
  }
  out[pos] = '\0';
  return pos;
}
