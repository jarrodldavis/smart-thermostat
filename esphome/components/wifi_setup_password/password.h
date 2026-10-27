// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>

namespace smeisner {
// 20 base32 characters from independent random bytes: 100 bits of entropy.
inline std::array<char, 21> setup_password(const uint8_t *random) {
  static constexpr char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  static_assert(sizeof(ALPHABET) == 33);
  std::array<char, 21> password{};
  for (unsigned n = 0; n < 20; ++n) password[n] = ALPHABET[random[n] & 31];
  return password;
}
}  // namespace smeisner
