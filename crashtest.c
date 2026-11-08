#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

#define TEST_KEY 0xCAFEULL
#define TEST_FILE "crash_file.txt"

static void
usage(void)
{
  printf(1, "Usage: crashtest <init|run <n>|replay|check>\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int fd, r, n;
  char buf[32];

  if(argc < 2)
    usage();

  if(strcmp(argv[1], "init") == 0){
    // 1. Create known starting state: file containing "A"
    unlink(TEST_FILE);
    fd = open(TEST_FILE, O_CREATE | O_WRONLY);
    if(fd < 0){
      printf(1, "INIT: FAIL (open)\n");
      exit();
    }
    if(write(fd, "A", 1) != 1){
      printf(1, "INIT: FAIL (write)\n");
      close(fd);
      exit();
    }
    close(fd);
    printf(1, "INIT: PASS (created %s with 'A')\n", TEST_FILE);
    exit();
  }

  if(strcmp(argv[1], "run") == 0){
    int point;
    if(argc < 3){
      printf(1, "Usage: crashtest run <point_number>\n");
      exit();
    }
    point = atoi(argv[2]);
    printf(1, "CRASHTEST: arming crashat(%d) and executing append 'B'...\n", point);
    crashat(point);
    r = effect(TEST_KEY, EFFECT_APPEND, TEST_FILE, "B", 1);
    printf(1, "CRASHTEST: completed without crash! (ret=%d)\n", r);
    exit();
  }

  if(strcmp(argv[1], "replay") == 0){
    // Replay the exact same request with fixed key and payload "B"
    r = effect(TEST_KEY, EFFECT_APPEND, TEST_FILE, "B", 1);
    printf(1, "REPLAY_RET: %d\n", r);

    fd = open(TEST_FILE, O_RDONLY);
    if(fd < 0){
      printf(1, "REPLAY: FAIL (cannot open %s)\n", TEST_FILE);
      exit();
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    printf(1, "FINAL_LEN: %d\n", n);
    printf(1, "FINAL_CONTENT: %s\n", buf);

    if(n == 2 && buf[0] == 'A' && buf[1] == 'B'){
      printf(1, "CRASH_TEST_RESULT: PASS (exactly 'AB')\n");
    } else {
      printf(1, "CRASH_TEST_RESULT: FAIL (expected 'AB')\n");
    }
    exit();
  }

  if(strcmp(argv[1], "check") == 0){
    fd = open(TEST_FILE, O_RDONLY);
    if(fd < 0){
      printf(1, "CHECK: file not found\n");
      exit();
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    printf(1, "LEN: %d, CONTENT: %s\n", n, buf);
    exit();
  }

  usage();
  exit();
}
