// SPDX-License-Identifier: GPL-3.0-only
//
// T-1.6 / T-1.10: smallest native test that proves lib/render/ compiles on the
// host with no Arduino/ESP/FreeRTOS present. If this ever needs those headers
// to build, the purity rule is already being violated for real. Real pixel
// parity tests arrive at T-2.6.

#include <unity.h>

#include "render.h"

void test_render_header_is_host_compilable(void) {
  // Just referencing a stdint type forces the translation unit to be real.
  volatile uint16_t pixel = 0xFFFFu;
  TEST_ASSERT_EQUAL_UINT16(0xFFFFu, pixel);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_header_is_host_compilable);
  return UNITY_END();
}
