#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

#define EFFECTCTL_KEY_LEN 16
#define EFFECTCTL_OUTPUT_SIZE 64
#define TEST_PATH "/agentlog.txt"
#define TEST_CREATE_PATH "/inbox.txt"

static int parse_effect_result(const char *text);

static int
raw_effect(unsigned long long key, int op, char *path, char *data, int n)
{
  return effect(key, op, path, data, n);
}

static int
run_effectctl(unsigned long long key, int op, char *path, char *data, int n)
{
  static char digits[] = "0123456789abcdef";
  static char prefix[] = "EFFECT_RESULT:";
  char key_text[EFFECTCTL_KEY_LEN + 1];
  char hex_data[2 * 512 + 1];
  char output[EFFECTCTL_OUTPUT_SIZE];
  char *create_argv[] = {"effectctl", "create", key_text, path, 0};
  char *append_argv[] = {"effectctl", "append", key_text, path, hex_data, 0};
  int descriptors[2], pid, i, used, count, result;

  for(i = EFFECTCTL_KEY_LEN - 1; i >= 0; i--){
    key_text[i] = digits[key & 0xF];
    key >>= 4;
  }
  key_text[EFFECTCTL_KEY_LEN] = 0;
  if(op == EFFECT_APPEND){
    if(n < 0 || n > 512)
      return -1;
    for(i = 0; i < n; i++){
      hex_data[2 * i] = digits[((uchar)data[i] >> 4) & 0xF];
      hex_data[2 * i + 1] = digits[(uchar)data[i] & 0xF];
    }
    hex_data[2 * n] = 0;
  }
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
    if(op == EFFECT_CREATE)
      exec("effectctl", create_argv);
    else if(op == EFFECT_APPEND)
      exec("effectctl", append_argv);
    exit();
  }

  close(descriptors[1]);
  used = 0;
  while(used < EFFECTCTL_OUTPUT_SIZE - 1 &&
        (count = read(descriptors[0], output + used,
                      EFFECTCTL_OUTPUT_SIZE - 1 - used)) > 0)
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

#define effect run_effectctl

// Local memcmp implementation for xv6 user space
static int
memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1 = (const uchar*)v1;
  const uchar *s2 = (const uchar*)v2;
  while(n-- > 0){
    if(*s1 != *s2)
      return *s1 - *s2;
    s1++;
    s2++;
  }
  return 0;
}

// Helper to check file contents and length
static int
verify_file(const char *path, const char *expected_content, int expected_len)
{
  int fd, n;
  char buf[64];

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;

  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n != expected_len)
    return -2;

  if(expected_len > 0 && memcmp(buf, expected_content, expected_len) != 0)
    return -3;

  return 0;
}

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

// =====================================================================
// Test 1: New Key & Duplicate Key (Sequential Append)
// =====================================================================
void
test_new_and_duplicate_key(unsigned long long key)
{
  int r1, r2;

  printf(1, "[TEST 3] New key & duplicate key append... ");

  // New key: first call must execute effect
  r1 = effect(key, EFFECT_APPEND, TEST_PATH, "hello", 5);
  if(r1 != 5){
    printf(1, "FAIL (new key append returned %d, expected 5)\n", r1);
    exit();
  }

  // Duplicate key: second call must return saved result without re-executing
  r2 = effect(key, EFFECT_APPEND, TEST_PATH, "hello", 5);
  if(r2 != 5){
    printf(1, "FAIL (duplicate key append returned %d, expected 5)\n", r2);
    exit();
  }

  // Verify file has "hello" (length 5), not "hellohello" (length 10)
  if(verify_file(TEST_PATH, "hello", 5) != 0){
    printf(1, "FAIL (file content or length mismatch)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 2: Different Keys on Same File
// =====================================================================
void
test_different_keys(unsigned long long key1, unsigned long long key2)
{
  int r1, r2;

  printf(1, "[TEST 4] Different keys sequential execution... ");

  // First key: appends "foo" (3 bytes)
  r1 = effect(key1, EFFECT_APPEND, TEST_PATH, "foo", 3);
  if(r1 != 3){
    printf(1, "FAIL (key1 returned %d)\n", r1);
    exit();
  }

  // Different key: must execute its own effect and append "bar" (3 bytes)
  r2 = effect(key2, EFFECT_APPEND, TEST_PATH, "bar", 3);
  if(r2 != 3){
    printf(1, "FAIL (key2 returned %d)\n", r2);
    exit();
  }

  // File must contain "foobar" (length 6)
  if(verify_file(TEST_PATH, "hellofoobar", 11) != 0){
    printf(1, "FAIL (file does not contain foobar)\n");
    exit();
  }

  // Replaying key1 and key2 must not append again
  r1 = effect(key1, EFFECT_APPEND, TEST_PATH, "foo", 3);
  r2 = effect(key2, EFFECT_APPEND, TEST_PATH, "bar", 3);
  if(r1 != 3 || r2 != 3 ||
     verify_file(TEST_PATH, "hellofoobar", 11) != 0){
    printf(1, "FAIL (replay modified file)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 3: Missing Files Error Handling & Idempotency
// =====================================================================
void
test_missing_file_error(unsigned long long key)
{
  int r1, r2;

  printf(1, "[TEST 1] Missing file error & duplicate retry... ");

  // Attempt to append to a file that does not exist -> fails (-1)
  r1 = effect(key, EFFECT_APPEND, TEST_PATH, "data", 4);
  if(r1 != -1){
    printf(1, "FAIL (append to missing file returned %d, expected -1)\n", r1);
    exit();
  }

  // Duplicate retry with same key -> must return saved failure (-1)
  r2 = effect(key, EFFECT_APPEND, TEST_PATH, "data", 4);
  if(r2 != -1){
    printf(1, "FAIL (duplicate retry returned %d, expected -1)\n", r2);
    exit();
  }

  // Create the file now
  r1 = effect(key + 1, EFFECT_CREATE, TEST_PATH, 0, 0);
  if(r1 != 0){
    printf(1, "FAIL (CREATE after missing-file result returned %d)\n", r1);
    exit();
  }

  // Third retry with the same key must STILL return -1 and NOT append
  r2 = effect(key, EFFECT_APPEND, TEST_PATH, "data", 4);
  if(r2 != -1 || verify_file(TEST_PATH, "", 0) != 0){
    printf(1, "FAIL (replaying failed key modified newly created file)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 4: CREATE Operations (New file, Replay, Existing file, Bad path)
// =====================================================================
void
test_create_operations(unsigned long long key_new, unsigned long long key_exist, unsigned long long key_bad)
{
  int r, fd;

  printf(1, "[TEST 2] CREATE operations & error handling... ");

  // 1. Create a brand-new file -> returns 0
  r = effect(key_new, EFFECT_CREATE, TEST_CREATE_PATH, 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE new file returned %d)\n", r);
    exit();
  }

  // Verify file was created
  fd = open(TEST_CREATE_PATH, O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (created file not found)\n");
    exit();
  }
  close(fd);

  // 2. Replay CREATE with same key -> returns 0
  r = effect(key_new, EFFECT_CREATE, TEST_CREATE_PATH, 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE replay returned %d)\n", r);
    exit();
  }

  // 3. CREATE on an already existing file with a NEW key -> returns 0 without error
  r = effect(key_exist, EFFECT_CREATE, TEST_CREATE_PATH, 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE on existing file returned %d)\n", r);
    exit();
  }

  // 4. CREATE on a path outside the kernel allowlist is denied.
  r = effect(key_bad, EFFECT_CREATE, "/not-allowed", 0, 0);
  if(r != -1){
    printf(1, "FAIL (CREATE on bad path returned %d, expected -1)\n", r);
    exit();
  }

  // Repeating a denied operation remains denied; it is not entered in the EPT.
  r = effect(key_bad, EFFECT_CREATE, "/not-allowed", 0, 0);
  if(r != -1){
    printf(1, "FAIL (CREATE bad path replay returned %d)\n", r);
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 5: Empty Payloads (n == 0)
// =====================================================================
void
test_empty_payloads(unsigned long long key1, unsigned long long key2)
{
  int r;

  printf(1, "[TEST 5] Empty append rejection and empty CREATE... ");

  r = effect(key1, EFFECT_APPEND, TEST_PATH, "", 0);
  if(r != -1){
    printf(1, "FAIL (empty append returned %d, expected -1)\n", r);
    exit();
  }

  // The rejected key must remain unused and may be used for a valid append.
  r = effect(key1, EFFECT_APPEND, TEST_PATH, "E", 1);
  if(r != 1 || verify_file(TEST_PATH, "hellofoobarE", 12) != 0){
    printf(1, "FAIL (rejected append consumed key or changed file)\n");
    exit();
  }
  r = effect(key1, EFFECT_APPEND, TEST_PATH, "E", 1);
  if(r != 1 || verify_file(TEST_PATH, "hellofoobarE", 12) != 0){
    printf(1, "FAIL (valid append replay changed file)\n");
    exit();
  }

  r = effect(key2, EFFECT_CREATE, TEST_PATH, 0, 0);
  if(r != 0 || verify_file(TEST_PATH, "hellofoobarE", 12) != 0){
    printf(1, "FAIL (empty CREATE arguments failed)\n");
    exit();
  }

  printf(1, "PASS\n");
}

void
test_kernel_action_policy(unsigned long long key)
{
  char oversized[21], nul_payload[2], malformed_utf8[2], unicode[2];
  char expected[18];
  int r, i;

  printf(1, "[TEST 6] Kernel path and payload policy... ");

  if(effect(key, EFFECT_APPEND, "/not-allowed", "V", 1) != -1 ||
     effect(key, EFFECT_APPEND, TEST_PATH, "V", 1) != 1){
    printf(1, "FAIL (denied path mutated or consumed the key)\n");
    exit();
  }

  for(i = 0; i < sizeof(oversized); i++)
    oversized[i] = 'a';
  if(effect(key + 1, EFFECT_APPEND, TEST_PATH, oversized,
            sizeof(oversized)) != -1 ||
     effect(key + 1, EFFECT_APPEND, TEST_PATH, "W", 1) != 1){
    printf(1, "FAIL (oversized append mutated or consumed the key)\n");
    exit();
  }

  nul_payload[0] = 'N';
  nul_payload[1] = 0;
  if(effect(key + 2, EFFECT_APPEND, TEST_PATH, nul_payload,
            sizeof(nul_payload)) != -1 ||
     effect(key + 2, EFFECT_APPEND, TEST_PATH, "X", 1) != 1){
    printf(1, "FAIL (NUL payload mutated or consumed the key)\n");
    exit();
  }

  malformed_utf8[0] = (char)0xc0;
  malformed_utf8[1] = (char)0x80;
  if(effect(key + 3, EFFECT_APPEND, TEST_PATH, malformed_utf8,
            sizeof(malformed_utf8)) != -1 ||
     effect(key + 3, EFFECT_APPEND, TEST_PATH, "Y", 1) != 1){
    printf(1, "FAIL (malformed UTF-8 mutated or consumed the key)\n");
    exit();
  }

  unicode[0] = (char)0xc3;
  unicode[1] = (char)0xa9;
  r = effect(key + 4, EFFECT_APPEND, TEST_PATH, unicode, sizeof(unicode));
  if(r != sizeof(unicode)){
    printf(1, "FAIL (valid UTF-8 append returned %d)\n", r);
    exit();
  }
  memmove(expected, "hellofoobarEVWXY", 16);
  expected[16] = unicode[0];
  expected[17] = unicode[1];
  if(verify_file(TEST_PATH, expected, sizeof(expected)) != 0){
    printf(1, "FAIL (kernel action policy content mismatch)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 6: Invalid Inputs & Error Validation
// =====================================================================
void
test_invalid_inputs(unsigned long long key)
{
  int r;

  printf(1, "[TEST 7] Direct syscall input and authorization validation... ");

  // 1. Key == 0 is forbidden
  r = raw_effect(0ULL, EFFECT_APPEND, TEST_PATH, "abc", 3);
  if(r != -1){
    printf(1, "FAIL (key 0 accepted)\n");
    exit();
  }

  // 2. Unknown op code
  r = raw_effect(key, 999, TEST_PATH, "abc", 3);
  if(r != -1){
    printf(1, "FAIL (invalid op accepted)\n");
    exit();
  }

  // 3. Negative op code
  r = raw_effect(key + 1, -1, TEST_PATH, "abc", 3);
  if(r != -1){
    printf(1, "FAIL (negative op accepted)\n");
    exit();
  }

  // 4. Payload size > BSIZE (512)
  r = raw_effect(key + 2, EFFECT_APPEND, TEST_PATH, "abc", 513);
  if(r != -1){
    printf(1, "FAIL (payload > BSIZE accepted)\n");
    exit();
  }

  // 5. Negative payload size
  r = raw_effect(key + 3, EFFECT_APPEND, TEST_PATH, "abc", -1);
  if(r != -1){
    printf(1, "FAIL (negative payload accepted)\n");
    exit();
  }

  // 6. Bad data pointer with n > 0
  r = raw_effect(key + 4, EFFECT_APPEND, TEST_PATH,
                 (char*)0xffffffff, 10);
  if(r != -1){
    printf(1, "FAIL (bad data pointer accepted)\n");
    exit();
  }

  // 7. Bad path pointer
  r = raw_effect(key + 5, EFFECT_APPEND, (char*)0xffffffff, "abc", 3);
  if(r != -1){
    printf(1, "FAIL (bad path pointer accepted)\n");
    exit();
  }

  // 8. crashat(0) disarm succeeds
  if(crashat(0) != 0){
    printf(1, "FAIL (crashat(0) failed)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 7: Key Reuse with Different Payload & Operation
// =====================================================================
void
test_key_reuse(unsigned long long key1, unsigned long long key2)
{
  int r1, r2;

  printf(1, "[TEST 8] Key reuse with different payload/op... ");

  // Key1 first call with "AAA"
  r1 = effect(key1, EFFECT_APPEND, TEST_PATH, "AAA", 3);
  if(r1 != 3){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Key1 reuse with different payload "BBB": must return saved 3, not write BBB
  r2 = effect(key1, EFFECT_APPEND, TEST_PATH, "BBB", 3);
  if(r2 != 3 ||
     verify_file(TEST_PATH, "hellofoobarEVWXY\xc3\xa9" "AAA", 21) != 0){
    printf(1, "FAIL (key reuse with different payload altered file)\n");
    exit();
  }

  // Key2 first call with APPEND
  r1 = effect(key2, EFFECT_APPEND, TEST_PATH, "DATA", 4);
  if(r1 != 4){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Key2 reuse with CREATE on another file: must return saved 4, not create new file
  r2 = effect(key2, EFFECT_CREATE, TEST_CREATE_PATH, 0, 0);
  if(r2 != 4){
    printf(1, "FAIL (key reuse with different op returned %d)\n", r2);
    exit();
  }
  printf(1, "PASS\n");
}

// =====================================================================
// Test 8: Concurrency Checkpoint (Two Processes Racing on Same Key)
// =====================================================================
void
test_concurrency_race(unsigned long long key)
{
  int pid;
  int p[2];
  int r_parent, r_child;

  printf(1, "[TEST 9] Concurrency race (two processes, same key)... ");

  if(pipe(p) < 0){
    printf(1, "FAIL (pipe)\n");
    exit();
  }

  pid = fork();
  if(pid < 0){
    printf(1, "FAIL (fork)\n");
    exit();
  }

  if(pid == 0){
    close(p[0]);
    r_child = effect(key, EFFECT_APPEND, TEST_PATH, "W", 1);
    write(p[1], &r_child, sizeof(r_child));
    close(p[1]);
    exit();
  }

  close(p[1]);
  r_parent = effect(key, EFFECT_APPEND, TEST_PATH, "W", 1);

  r_child = 0;
  read(p[0], &r_child, sizeof(r_child));
  close(p[0]);
  wait();

  if(r_parent != 1 || r_child != 1){
    printf(1, "FAIL (parent=%d child=%d, expected both 1)\n", r_parent, r_child);
    exit();
  }

  if(verify_file(TEST_PATH, "hellofoobarEVWXY\xc3\xa9" "AAADATAW", 26) != 0){
    printf(1, "FAIL (effect executed more than once concurrently)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Test 9: Multi-Worker Concurrency (4 Children Racing on Same Key)
// =====================================================================
void
test_multi_concurrency(unsigned long long key)
{
  int pids[4];
  int pipes[4][2];
  int i, r;

  printf(1, "[TEST 10] Multi-worker concurrency (4 processes, same key)... ");

  for(i = 0; i < 4; i++){
    if(pipe(pipes[i]) < 0){
      printf(1, "FAIL (pipe %d)\n", i);
      exit();
    }
  }

  for(i = 0; i < 4; i++){
    pids[i] = fork();
    if(pids[i] < 0){
      printf(1, "FAIL (fork %d)\n", i);
      exit();
    }
    if(pids[i] == 0){
      close(pipes[i][0]);
      r = effect(key, EFFECT_APPEND, TEST_PATH, "M", 1);
      write(pipes[i][1], &r, sizeof(r));
      close(pipes[i][1]);
      exit();
    }
    close(pipes[i][1]);
  }

  for(i = 0; i < 4; i++){
    r = 0;
    read(pipes[i][0], &r, sizeof(r));
    close(pipes[i][0]);
    wait();
    if(r != 1){
      printf(1, "FAIL (worker %d returned %d)\n", i, r);
      exit();
    }
  }

  if(verify_file(TEST_PATH, "hellofoobarEVWXY\xc3\xa9" "AAADATAWM", 27) != 0){
    printf(1, "FAIL (multiple workers executed effect)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// =====================================================================
// Main Test Runner
// =====================================================================
int
main(void)
{
  unsigned long long base;

  // Use uptime ticks to derive unique non-colliding base keys per test run
  base = ((unsigned long long)uptime() + 1) * 100000ULL;

  printf(1, "\n============================================\n");
  printf(1, "   XV6 EFFECT() PHASE 5 FUNCTIONAL TESTS   \n");
  printf(1, "============================================\n\n");

  if(unlink(TEST_CREATE_PATH) < 0){
    printf(1, "FAIL (could not reset CREATE fixture)\n");
    exit();
  }
  test_missing_file_error(base + 30);
  test_create_operations(base + 40, base + 41, base + 42);
  test_new_and_duplicate_key(base + 10);
  test_different_keys(base + 20, base + 21);
  test_empty_payloads(base + 50, base + 51);
  test_kernel_action_policy(base + 60);
  test_invalid_inputs(base + 70);
  test_key_reuse(base + 70, base + 71);
  test_concurrency_race(base + 80);
  test_multi_concurrency(base + 90);

  printf(1, "\n============================================\n");
  printf(1, "   ALL PHASE 5 FUNCTIONAL TESTS PASSED!    \n");
  printf(1, "============================================\n\n");

  exit();
}
