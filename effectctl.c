#include "types.h"
#include "user.h"
#include "effect.h"

#define EFFECTCTL_MAX_DATA 512

static int
hexval(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int
parse_key(const char *s, unsigned long long *key)
{
  int i, v;
  unsigned long long value = 0;

  if(strlen(s) != 16)
    return -1;
  for(i = 0; i < 16; i++){
    v = hexval(s[i]);
    if(v < 0)
      return -1;
    value = (value << 4) | (unsigned long long)v;
  }
  if(value == 0)
    return -1;
  *key = value;
  return 0;
}

static int
hex_decode(const char *src, char *dst, int capacity)
{
  int i, hi, lo, n = strlen(src);
  if((n & 1) || n / 2 > capacity)
    return -1;
  for(i = 0; i < n; i += 2){
    hi = hexval(src[i]);
    lo = hexval(src[i + 1]);
    if(hi < 0 || lo < 0)
      return -1;
    dst[i / 2] = (char)((hi << 4) | lo);
  }
  return n / 2;
}

int
main(int argc, char *argv[])
{
  unsigned long long key;
  char data[EFFECTCTL_MAX_DATA];
  int op, n, result;

  if(argc < 3 || parse_key(argv[2], &key) < 0){
    printf(2, "usage: effectctl append KEY16 PATH HEXDATA | create KEY16 PATH\n");
    exit();
  }

  if(strcmp(argv[1], "create") == 0 && argc == 4){
    op = EFFECT_CREATE;
    result = effect(key, op, argv[3], 0, 0);
  } else if(strcmp(argv[1], "append") == 0 && argc == 5){
    op = EFFECT_APPEND;
    n = hex_decode(argv[4], data, sizeof(data));
    if(n < 0){
      printf(2, "invalid hex payload or payload too large\n");
      exit();
    }
    result = effect(key, op, argv[3], data, n);
  } else {
    printf(2, "usage: effectctl append KEY16 PATH HEXDATA | create KEY16 PATH\n");
    exit();
  }

  printf(1, "EFFECT_RESULT:%d\n", result);
  exit();
}
