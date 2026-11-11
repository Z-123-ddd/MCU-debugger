#include <stdint.h>

typedef struct { float x, y; uint16_t flags; } Point;
typedef enum { MODE_IDLE = 0, MODE_RUN = 1, MODE_FAULT = 2 } Mode;

volatile float    g_temperature = 25.5f;
volatile int32_t  g_counter     = 0;
volatile uint16_t g_adc_raw[4]  = {100, 200, 300, 400};
volatile Point    g_point       = {1.5f, -2.5f, 0xA5A5};
volatile Mode     g_mode        = MODE_IDLE;
volatile double   g_voltage     = 3.3;
volatile uint8_t  g_flags       = 0;
static  int32_t   g_static_only = 42;
volatile const float g_const_val = 1.23f;

int main(void) {
    while (1) { g_counter++; }
    return 0;
}
