#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

#define TEST_BASE_KEY 0xCAFE0000ULL
#define TEST_FILE "/agentlog.txt"

static int
parse_effect_result(const char *text)
{
  int sign, result;

  sign = 1;
  if(*text == '-'){
    sign = -1;
    text++;
  }
  result = 0;
  while('0' <= *text && *text <= '9')
    result = result * 10 + *text++ - '0';
  return sign * result;
}

static int
run_effectctl(unsigned long long key, char *path, char *data)
{
  static char digits[] = "0123456789abcdef";
  static char prefix[] = "EFFECT_RESULT:";
  char key_text[17], hex_data[3], output[64];
  char *args[] = {"effectctl", "append", key_text, path, hex_data, 0};
  int descriptors[2], pid, i, used, count, result;

  for(i = 15; i >= 0; i--){
    key_text[i] = digits[key & 0xF];
    key >>= 4;
  }
  key_text[16] = 0;
  hex_data[0] = digits[((uchar)data[0] >> 4) & 0xF];
  hex_data[1] = digits[(uchar)data[0] & 0xF];
  hex_data[2] = 0;
  if(pipe(descriptors) < 0)
    return -1;
  pid = fork();
  if(pid < 0){
    close(descriptors[0]);
    close(descriptors[1]);
    return -1;
  }
  if(pid == 0){
    close(descriptors[0]);
    close(1);
    if(dup(descriptors[1]) != 1)
      exit();
    close(descriptors[1]);
    exec("effectctl", args);
    exit();
  }
  close(descriptors[1]);
  used = 0;
  while(used < sizeof(output) - 1 &&
        (count = read(descriptors[0], output + used,
                      sizeof(output) - 1 - used)) > 0)
    used += count;
  close(descriptors[0]);
  wait();
  output[used] = 0;
  for(i = 0; i < 14; i++)
    if(output[i] != prefix[i])
      return -1;
  result = parse_effect_result(output + 14);
  return result;
}

static void
usage(void)
{
  printf(1, "Usage: crashtest <init [point]|run <point>|replay [point]|check>\n");
  exit();
}

static unsigned long long
get_key(int argc, char *argv[], int idx)
{
  if(argc > idx){
    int p = atoi(argv[idx]);
    if(p > 0)
      return TEST_BASE_KEY + (unsigned long long)p;
  }
  return 0xCAFEULL;
}

int
main(int argc, char *argv[])
{
  int fd, r, n;
  char buf[32];
  unsigned long long key;

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
    key = get_key(argc, argv, 2);
    printf(1, "CRASHTEST: arming crashat(%d) [key=0x%x%x] and executing append 'B'...\n",
           point, (uint)(key >> 32), (uint)key);
    crashat(point);
    r = run_effectctl(key, TEST_FILE, "B");
    printf(1, "CRASHTEST: completed without crash! (ret=%d)\n", r);
    exit();
  }

  if(strcmp(argv[1], "replay") == 0){
    // Replay the exact same request with fixed key and payload "B"
    key = get_key(argc, argv, 2);
    r = run_effectctl(key, TEST_FILE, "B");
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
