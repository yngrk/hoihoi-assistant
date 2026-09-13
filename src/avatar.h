#pragma once

// HoiHoi als Bild: 400x300, ein Bit je Pixel, Zeile fuer Zeile, hoechstes Bit
// links, 1 = schwarz. Erzeugt von tools/avatar.py aus assets/avatar.png.

#include <stdint.h>

const int kAvatarBreite = 400;
const int kAvatarHoehe  = 300;

extern const uint8_t kAvatar[(kAvatarBreite + 7) / 8 * kAvatarHoehe];
