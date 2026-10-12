/* Generated from upstream rsync 3.4.1's get_checksum1() and sum_sizes_sqroot()
 * (a throwaway oracle program, deliberately not kept in this tree). Do not edit. */

/* Input: buf[i] = (uint8_t)(i * 37 + 11). */
static const struct { int len; uint32_t sum1; } sum1_vec[] = {
  { 0, 0x00000000UL },
  { 1, 0x000b000bUL },
  { 2, 0x0046003bUL },
  { 3, 0x00d60090UL },
  { 4, 0x01e0010aUL },
  { 5, 0x028900a9UL },
  { 7, 0x034c0056UL },
  { 8, 0x03b00064UL },
  { 9, 0x04470097UL },
  { 64, 0xfe80ff20UL },
  { 700, 0x6a50ffa6UL },
  { 999, 0x9b1cfde6UL },
};

static const struct { int64_t len; int csum_length; int32_t blength; int s2length; } sizes_vec[] = {
  { 1LL, 2, 700, 2 },
  { 1LL, 16, 700, 16 },
  { 700LL, 2, 700, 2 },
  { 700LL, 16, 700, 16 },
  { 489999LL, 2, 700, 2 },
  { 489999LL, 16, 700, 16 },
  { 490000LL, 2, 700, 2 },
  { 490000LL, 16, 700, 16 },
  { 490001LL, 2, 700, 2 },
  { 490001LL, 16, 700, 16 },
  { 1000000LL, 2, 1000, 2 },
  { 1000000LL, 16, 1000, 16 },
  { 10000000LL, 2, 3160, 2 },
  { 10000000LL, 16, 3160, 16 },
  { 123456789LL, 2, 11104, 3 },
  { 123456789LL, 16, 11104, 16 },
  { 2147483648LL, 2, 46336, 4 },
  { 2147483648LL, 16, 46336, 16 },
  { 5000000000LL, 2, 70704, 4 },
  { 5000000000LL, 16, 70704, 16 },
  { 1099511627776LL, 2, 131072, 6 },
  { 1099511627776LL, 16, 131072, 16 },
  { 1125899906842624LL, 2, 131072, 8 },
  { 1125899906842624LL, 16, 131072, 16 },
};
