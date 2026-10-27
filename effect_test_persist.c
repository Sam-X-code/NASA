
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

int
main(int argc, char *argv[])
{
  char buf[64];
  int fd, r, n;

  if(argc > 1){
    // Run after reboot: retry the original key.
    r = effect(12345ULL, EFFECT_APPEND,
               "effect_data.txt", "hello", 5);

    fd = open("effect_data.txt", O_RDONLY);
    if(fd < 0){
      printf(1, "PERSISTENCE TEST: FAIL (open)\n");
      exit();
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);

    if(r == 5 && n == 5 &&
       buf[0] == 'h' && buf[1] == 'e' &&
       buf[2] == 'l' && buf[3] == 'l' &&
       buf[4] == 'o')
      printf(1, "PERSISTENCE TEST: PASS\n");
    else
      printf(1, "PERSISTENCE TEST: FAIL r=%d n=%d\n", r, n);

    exit();
  }

  // First run: create file and append once.
  fd = open("effect_data.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "SETUP FAIL\n");
    exit();
  }
  close(fd);

  r = effect(12345ULL, EFFECT_APPEND,
             "effect_data.txt", "hello", 5);

  if(r == 5)
    printf(1, "FIRST RUN: PASS — reboot now\n");
  else
    printf(1, "FIRST RUN: FAIL r=%d\n", r);

  exit();
}
