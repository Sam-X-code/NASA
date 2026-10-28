/*#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

int
main(void)
{
  int r1, r2, fd, n;
  char buf[16];

  // First attempt: file does not exist.
  r1 = effect(33333ULL, EFFECT_APPEND,
              "missing.txt", "hello", 5);

  // Create the file afterwards using a normal syscall.
  fd = open("missing.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "TEST FAIL: create file\n");
    exit();
  }
  close(fd);

  // Same key must return the original failure.
  r2 = effect(33333ULL, EFFECT_APPEND,
              "missing.txt", "hello", 5);

  fd = open("missing.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "TEST FAIL: reopen file\n");
    exit();
  }
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(r1 == -1 && r2 == -1 && n == 0)
    printf(1, "FAILURE RETRY TEST: PASS\n");
  else
    printf(1, "FAILURE RETRY TEST: FAIL r1=%d r2=%d n=%d\n",
           r1, r2, n);

  exit();
}
*/


#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

int
main(void)
{
  int r, fd, n;
  char buf[16];

  // First call: append and save the result under this key.
  fd = open("crash_data.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "TEST FAIL: create file\n");
    exit();
  }
  close(fd);

  r = effect(44444ULL, EFFECT_APPEND,
             "crash_data.txt", "hello", 5);

  if(r != 5){
    printf(1, "TEST FAIL: first append returned %d\n", r);
    exit();
  }

  // Retry the same key: must not append again.
  r = effect(44444ULL, EFFECT_APPEND,
             "crash_data.txt", "hello", 5);

  fd = open("crash_data.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "TEST FAIL: reopen file\n");
    exit();
  }
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(r == 5 && n == 5 &&
     buf[0] == 'h' && buf[1] == 'e' &&
     buf[2] == 'l' && buf[3] == 'l' &&
     buf[4] == 'o')
    printf(1, "EXACTLY ONCE APPEND TEST: PASS\n");
  else
    printf(1, "EXACTLY ONCE APPEND TEST: FAIL r=%d n=%d\n",
           r, n);

  exit();
}
