#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

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

// =====================================================================
// Test 1: New Key & Duplicate Key (Sequential Append)
// =====================================================================
void
test_new_and_duplicate_key(unsigned long long key)
{
  int r1, r2, fd;

  printf(1, "[TEST 1] New key & duplicate key append... ");

  unlink("test_new_dup.txt");
  fd = open("test_new_dup.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  close(fd);

  // New key: first call must execute effect
  r1 = effect(key, EFFECT_APPEND, "test_new_dup.txt", "hello", 5);
  if(r1 != 5){
    printf(1, "FAIL (new key append returned %d, expected 5)\n", r1);
    exit();
  }

  // Duplicate key: second call must return saved result without re-executing
  r2 = effect(key, EFFECT_APPEND, "test_new_dup.txt", "hello", 5);
  if(r2 != 5){
    printf(1, "FAIL (duplicate key append returned %d, expected 5)\n", r2);
    exit();
  }

  // Verify file has "hello" (length 5), not "hellohello" (length 10)
  if(verify_file("test_new_dup.txt", "hello", 5) != 0){
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
  int r1, r2, fd;

  printf(1, "[TEST 2] Different keys sequential execution... ");

  unlink("test_diff_keys.txt");
  fd = open("test_diff_keys.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  close(fd);

  // First key: appends "foo" (3 bytes)
  r1 = effect(key1, EFFECT_APPEND, "test_diff_keys.txt", "foo", 3);
  if(r1 != 3){
    printf(1, "FAIL (key1 returned %d)\n", r1);
    exit();
  }

  // Different key: must execute its own effect and append "bar" (3 bytes)
  r2 = effect(key2, EFFECT_APPEND, "test_diff_keys.txt", "bar", 3);
  if(r2 != 3){
    printf(1, "FAIL (key2 returned %d)\n", r2);
    exit();
  }

  // File must contain "foobar" (length 6)
  if(verify_file("test_diff_keys.txt", "foobar", 6) != 0){
    printf(1, "FAIL (file does not contain foobar)\n");
    exit();
  }

  // Replaying key1 and key2 must not append again
  r1 = effect(key1, EFFECT_APPEND, "test_diff_keys.txt", "foo", 3);
  r2 = effect(key2, EFFECT_APPEND, "test_diff_keys.txt", "bar", 3);
  if(r1 != 3 || r2 != 3 || verify_file("test_diff_keys.txt", "foobar", 6) != 0){
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
  int r1, r2, fd;

  printf(1, "[TEST 3] Missing file error & duplicate retry... ");

  unlink("test_missing.txt");

  // Attempt to append to a file that does not exist -> fails (-1)
  r1 = effect(key, EFFECT_APPEND, "test_missing.txt", "data", 4);
  if(r1 != -1){
    printf(1, "FAIL (append to missing file returned %d, expected -1)\n", r1);
    exit();
  }

  // Duplicate retry with same key -> must return saved failure (-1)
  r2 = effect(key, EFFECT_APPEND, "test_missing.txt", "data", 4);
  if(r2 != -1){
    printf(1, "FAIL (duplicate retry returned %d, expected -1)\n", r2);
    exit();
  }

  // Create the file now
  fd = open("test_missing.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (open create)\n");
    exit();
  }
  close(fd);

  // Third retry with the same key must STILL return -1 and NOT append
  r2 = effect(key, EFFECT_APPEND, "test_missing.txt", "data", 4);
  if(r2 != -1 || verify_file("test_missing.txt", "", 0) != 0){
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

  printf(1, "[TEST 4] CREATE operations & error handling... ");

  unlink("test_create_new.txt");

  // 1. Create a brand-new file -> returns 0
  r = effect(key_new, EFFECT_CREATE, "test_create_new.txt", 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE new file returned %d)\n", r);
    exit();
  }

  // Verify file was created
  fd = open("test_create_new.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (created file not found)\n");
    exit();
  }
  close(fd);

  // 2. Replay CREATE with same key -> returns 0
  r = effect(key_new, EFFECT_CREATE, "test_create_new.txt", 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE replay returned %d)\n", r);
    exit();
  }

  // 3. CREATE on an already existing file with a NEW key -> returns 0 without error
  r = effect(key_exist, EFFECT_CREATE, "test_create_new.txt", 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE on existing file returned %d)\n", r);
    exit();
  }

  // 4. CREATE on invalid path (non-existent parent directory) -> returns -1
  r = effect(key_bad, EFFECT_CREATE, "nodir/test.txt", 0, 0);
  if(r != -1){
    printf(1, "FAIL (CREATE on bad path returned %d, expected -1)\n", r);
    exit();
  }

  // Replay on invalid path -> returns -1
  r = effect(key_bad, EFFECT_CREATE, "nodir/test.txt", 0, 0);
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
  int r, fd;

  printf(1, "[TEST 5] Empty payloads (0 bytes)... ");

  unlink("test_empty.txt");
  fd = open("test_empty.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  write(fd, "initial", 7);
  close(fd);

  // Append empty payload (n = 0) to existing file
  r = effect(key1, EFFECT_APPEND, "test_empty.txt", "", 0);
  if(r != 0){
    printf(1, "FAIL (empty append returned %d, expected 0)\n", r);
    exit();
  }

  // Replay empty append
  r = effect(key1, EFFECT_APPEND, "test_empty.txt", "", 0);
  if(r != 0){
    printf(1, "FAIL (empty append replay returned %d)\n", r);
    exit();
  }

  // File contents must still be exactly "initial" (length 7)
  if(verify_file("test_empty.txt", "initial", 7) != 0){
    printf(1, "FAIL (file altered by empty append)\n");
    exit();
  }

  // CREATE with 0 data and 0 length
  unlink("test_empty_create.txt");
  r = effect(key2, EFFECT_CREATE, "test_empty_create.txt", 0, 0);
  if(r != 0 || verify_file("test_empty_create.txt", "", 0) != 0){
    printf(1, "FAIL (empty CREATE failed)\n");
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

  printf(1, "[TEST 6] Invalid inputs validation... ");

  // 1. Key == 0 is forbidden
  r = effect(0ULL, EFFECT_APPEND, "test_new_dup.txt", "abc", 3);
  if(r != -1){
    printf(1, "FAIL (key 0 accepted)\n");
    exit();
  }

  // 2. Unknown op code
  r = effect(key, 999, "test_new_dup.txt", "abc", 3);
  if(r != -1){
    printf(1, "FAIL (invalid op accepted)\n");
    exit();
  }

  // 3. Negative op code
  r = effect(key + 1, -1, "test_new_dup.txt", "abc", 3);
  if(r != -1){
    printf(1, "FAIL (negative op accepted)\n");
    exit();
  }

  // 4. Payload size > BSIZE (512)
  r = effect(key + 2, EFFECT_APPEND, "test_new_dup.txt", "abc", 513);
  if(r != -1){
    printf(1, "FAIL (payload > BSIZE accepted)\n");
    exit();
  }

  // 5. Negative payload size
  r = effect(key + 3, EFFECT_APPEND, "test_new_dup.txt", "abc", -1);
  if(r != -1){
    printf(1, "FAIL (negative payload accepted)\n");
    exit();
  }

  // 6. Bad data pointer with n > 0
  r = effect(key + 4, EFFECT_APPEND, "test_new_dup.txt", (char*)0xffffffff, 10);
  if(r != -1){
    printf(1, "FAIL (bad data pointer accepted)\n");
    exit();
  }

  // 7. Bad path pointer
  r = effect(key + 5, EFFECT_APPEND, (char*)0xffffffff, "abc", 3);
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
  int r1, r2, fd;

  printf(1, "[TEST 7] Key reuse with different payload/op... ");

  unlink("test_reuse_1.txt");
  fd = open("test_reuse_1.txt", O_CREATE | O_WRONLY);
  close(fd);

  // Key1 first call with "AAA"
  r1 = effect(key1, EFFECT_APPEND, "test_reuse_1.txt", "AAA", 3);
  if(r1 != 3){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Key1 reuse with different payload "BBB": must return saved 3, not write BBB
  r2 = effect(key1, EFFECT_APPEND, "test_reuse_1.txt", "BBB", 3);
  if(r2 != 3 || verify_file("test_reuse_1.txt", "AAA", 3) != 0){
    printf(1, "FAIL (key reuse with different payload altered file)\n");
    exit();
  }

  // Key2 first call with APPEND
  unlink("test_reuse_2.txt");
  fd = open("test_reuse_2.txt", O_CREATE | O_WRONLY);
  close(fd);
  r1 = effect(key2, EFFECT_APPEND, "test_reuse_2.txt", "DATA", 4);
  if(r1 != 4){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Key2 reuse with CREATE on another file: must return saved 4, not create new file
  unlink("test_should_not_exist.txt");
  r2 = effect(key2, EFFECT_CREATE, "test_should_not_exist.txt", 0, 0);
  if(r2 != 4){
    printf(1, "FAIL (key reuse with different op returned %d)\n", r2);
    exit();
  }
  fd = open("test_should_not_exist.txt", O_RDONLY);
  if(fd >= 0){
    close(fd);
    printf(1, "FAIL (new file was created on key reuse)\n");
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
  int pid, fd;
  int p[2];
  int r_parent, r_child;

  printf(1, "[TEST 8] Concurrency race (two processes, same key)... ");

  unlink("test_conc_race.txt");
  fd = open("test_conc_race.txt", O_CREATE | O_WRONLY);
  close(fd);

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
    r_child = effect(key, EFFECT_APPEND, "test_conc_race.txt", "W", 1);
    write(p[1], &r_child, sizeof(r_child));
    close(p[1]);
    exit();
  }

  close(p[1]);
  r_parent = effect(key, EFFECT_APPEND, "test_conc_race.txt", "W", 1);

  r_child = 0;
  read(p[0], &r_child, sizeof(r_child));
  close(p[0]);
  wait();

  if(r_parent != 1 || r_child != 1){
    printf(1, "FAIL (parent=%d child=%d, expected both 1)\n", r_parent, r_child);
    exit();
  }

  // File size MUST be exactly 1 byte ("W"), NOT 2 bytes ("WW")
  if(verify_file("test_conc_race.txt", "W", 1) != 0){
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
  int i, fd, r;

  printf(1, "[TEST 9] Multi-worker concurrency (4 processes, same key)... ");

  unlink("test_multi_race.txt");
  fd = open("test_multi_race.txt", O_CREATE | O_WRONLY);
  close(fd);

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
      r = effect(key, EFFECT_APPEND, "test_multi_race.txt", "M", 1);
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

  // File size MUST be exactly 1 byte ("M"), NOT 4 bytes ("MMMM")
  if(verify_file("test_multi_race.txt", "M", 1) != 0){
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

  test_new_and_duplicate_key(base + 10);
  test_different_keys(base + 20, base + 21);
  test_missing_file_error(base + 30);
  test_create_operations(base + 40, base + 41, base + 42);
  test_empty_payloads(base + 50, base + 51);
  test_invalid_inputs(base + 60);
  test_key_reuse(base + 70, base + 71);
  test_concurrency_race(base + 80);
  test_multi_concurrency(base + 90);

  printf(1, "\n============================================\n");
  printf(1, "   ALL PHASE 5 FUNCTIONAL TESTS PASSED!    \n");
  printf(1, "============================================\n\n");

  exit();
}
