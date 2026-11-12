/* Fixture TU B — the referencing side, i.e. a second compilation unit.
 *
 * Its only purpose is to make the fixture a genuinely multi-CU image and to
 * reference `Point` through a declaration rather than a definition, which is the
 * shape that made a real ODrive .axf look like it had 3 variables: the type of
 * `controllers` is a bare `class Controller;` declaration in that unit, so a reader
 * that cannot follow it to the definition drops the variable entirely.
 */
#include <stdint.h>

typedef struct {
    float x, y;
    uint16_t flags;
} Point;

/* Declared here, defined in TU A: the DWARF entry in this unit is a declaration
 * with no size, so resolving it needs the definition from the other unit. */
extern volatile uint32_t g_shared;

volatile int32_t g_tu_b_counter = 7;
volatile Point   g_tu_b_point = {9.5f, -8.5f, 0x1234};

void tu_b_touch(void)
{
    g_shared++;
    g_tu_b_counter++;
}
