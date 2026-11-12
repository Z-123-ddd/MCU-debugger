/* Fixture TU A — the definition side.
 *
 * Compiled together with symbol_test_b.c to produce a MULTI-CU image, because a
 * single-unit fixture cannot catch the bugs that matter on a real C++ project:
 *
 *   1. stopping after the first compilation unit,
 *   2. failing to follow a forward declaration to the real definition,
 *   3. dropping an array-of-struct (no rows at all instead of members),
 *   4. emitting the same global once per unit that references it.
 *
 * Regenerate with: python scripts/gen-symbol-fixture.py
 */
#include <stdint.h>

typedef struct {
    float x, y;
    uint16_t flags;
} Point;

/* Array of struct: must expand to g_points[0].x … (a 1-element array of an
 * undefined type used to silently produce nothing at all). */
volatile Point g_points[2] = {{1.5f, -2.5f, 0xA5A5}, {3.5f, -4.5f, 0x5A5A}};

volatile float   g_temperature = 25.5f;
volatile int32_t g_counter     = 0;
volatile uint16_t g_adc_raw[4] = {100, 200, 300, 400};
volatile double  g_voltage     = 3.3;

/* Shared with TU B so the same object is described twice (dedupe case). */
volatile uint32_t g_shared = 0xDEADBEEF;

int main(void)
{
    while (1)
        g_counter++;
    return 0;
}
