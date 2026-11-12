#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

#define AUTH_CREATE_KEY 0xA1100001ULL
#define AUTH_APPEND_KEY 0xA1100002ULL
#define AUTH_ALIAS_KEY 0xA1100003ULL
#define AUTH_DOT_KEY 0xA1100004ULL
#define AUTH_TRAVERSAL_KEY 0xA1100005ULL
#define AUTH_REPLAY_KEY 0xA1000004ULL
#define AUTH_TAINTED_KEY 0xA1200001ULL

static int
bytes_equal(char *left, char *right, int count)
{
  int i;

  for(i = 0; i < count; i++)
    if(left[i] != right[i])
      return 0;
  return 1;
}

static int
check(int condition, char *message)
{
  if(condition){
    printf(1, "%s: PASS\n", message);
    return 0;
  }
  printf(1, "%s: FAIL\n", message);
  return -1;
}

static int
read_file(char *path, char *buf, int size)
{
  int fd, n;

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  n = read(fd, buf, size);
  close(fd);
  return n;
}

int
main(int argc, char *argv[])
{
  char before[32], after[32], input[64];
  int failed = 0, fd, n;

  if(argc == 2 && strcmp(argv[1], "replay") == 0){
    failed |= check(effect(AUTH_REPLAY_KEY, EFFECT_APPEND,
                           "/agentlog.txt", "B", 1) == -1,
                    "unauthorized existing-key replay denied");
    n = read_file("/agentlog.txt", after, sizeof(after));
    failed |= check(n == 2 && bytes_equal(after, "AB", 2),
                    "unauthorized replay leaves file unchanged");
    printf(1, "KERNEL_EFFECT_REPLAY_AUTH_TEST: %s\n",
           failed ? "FAIL" : "PASS");
    exit();
  }

  if(argc == 2 && strcmp(argv[1], "tainted") == 0){
    fd = open("/inbox.txt", O_RDONLY);
    if(fd < 0){
      printf(1, "taint fixture open: FAIL\n");
      exit();
    }
    read(fd, input, sizeof(input));
    close(fd);
    failed |= check(effect(AUTH_TAINTED_KEY, EFFECT_APPEND,
                           "/agentlog.txt", "X", 1) == -1,
                    "tainted non-principal direct effect denied");
    n = read_file("/agentlog.txt", after, sizeof(after));
    failed |= check(n == 2 && bytes_equal(after, "AB", 2),
                    "tainted direct effect leaves file unchanged");
    printf(1, "KERNEL_EFFECT_TAINT_AUTH_TEST: %s\n",
           failed ? "FAIL" : "PASS");
    exit();
  }

  unlink("/agentlog.txt");
  failed |= check(effect(AUTH_CREATE_KEY, EFFECT_CREATE,
                         "/agentlog.txt", 0, 0) == -1,
                  "unauthorized direct create denied");
  fd = open("/agentlog.txt", O_RDONLY);
  if(fd >= 0)
    close(fd);
  failed |= check(fd < 0, "unauthorized create leaves target absent");

  n = read_file("/important.txt", before, sizeof(before));
  failed |= check(n == 13 && bytes_equal(before, "DO NOT DELETE", 13),
                  "protected fixture initial contents verified");
  failed |= check(effect(AUTH_APPEND_KEY, EFFECT_APPEND,
                         "/important.txt", "X", 1) == -1,
                  "unauthorized direct append denied");
  failed |= check(effect(AUTH_ALIAS_KEY, EFFECT_APPEND,
                         "//important.txt", "X", 1) == -1,
                  "repeated-separator alias denied");
  failed |= check(effect(AUTH_DOT_KEY, EFFECT_APPEND,
                         "/./important.txt", "X", 1) == -1,
                  "dot-component alias denied");
  failed |= check(effect(AUTH_TRAVERSAL_KEY, EFFECT_CREATE,
                         "/agentlog.txt/../important.txt", 0, 0) == -1,
                  "dot-dot path alias denied");
  n = read_file("/important.txt", after, sizeof(after));
  failed |= check(n == 13 && bytes_equal(after, before, 13),
                  "unauthorized operations leave protected file unchanged");

  fd = open("/effectctl", O_WRONLY);
  if(fd >= 0)
    close(fd);
  failed |= check(fd < 0, "effect principal executable cannot be opened for write");
  failed |= check(unlink("/effectctl") == -1,
                  "effect principal executable cannot be unlinked");
  failed |= check(link("/effectctl", "/effectalias") == -1,
                  "effect principal executable cannot be hard-linked");

  printf(1, "KERNEL_EFFECT_AUTH_TEST: %s\n", failed ? "FAIL" : "PASS");
  exit();
}
