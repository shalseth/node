#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const uint32_t inputs[] = {0x3f8ccccd, 0x7fc00000, 0xffc00000, 0x7fa00000};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    float input;
    memcpy(&input, &inputs[i], sizeof(input));
    const float outputs[] = {ceilf(input), floorf(input), truncf(input)};
    printf("%08x", inputs[i]);
    for (unsigned n = 0; n < sizeof(outputs) / sizeof(outputs[0]); ++n) {
      uint32_t bits;
      memcpy(&bits, &outputs[n], sizeof(bits));
      printf(" %08x", bits);
    }
    putchar('\n');
  }
  return 0;
}
