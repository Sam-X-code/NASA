#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "effect.h"

// Test 1: Sequential exactly-once append
void
test_sequential_append(void)
{
  int r1, r2, fd, n;
  char buf[16];

  printf(1, "[TEST 1] Sequential exactly-once append... ");

  unlink("test_seq.txt");
  fd = open("test_seq.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  close(fd);

  // First call
  r1 = effect(10001ULL, EFFECT_APPEND, "test_seq.txt", "hello", 5);
  if(r1 != 5){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Duplicate call with same key
  r2 = effect(10001ULL, EFFECT_APPEND, "test_seq.txt", "hello", 5);
  if(r2 != 5){
    printf(1, "FAIL (duplicate append returned %d)\n", r2);
    exit();
  }

  // Verify file content
  fd = open("test_seq.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (reopen)\n");
    exit();
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n == 5 && strcmp(buf, "hello") == 0){
    printf(1, "PASS\n");
  } else {
    printf(1, "FAIL (file len=%d content=%s)\n", n, buf);
    exit();
  }
}

// Test 2: Key reuse with different payload
void
test_key_reuse_different_payload(void)
{
  int r1, r2, fd, n;
  char buf[32];

  printf(1, "[TEST 2] Key reuse with different payload... ");

  unlink("test_reuse_p.txt");
  fd = open("test_reuse_p.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  close(fd);

  // First call with "AAA"
  r1 = effect(10002ULL, EFFECT_APPEND, "test_reuse_p.txt", "AAA", 3);
  if(r1 != 3){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Same key with different payload "BBB"
  // Defined behavior: key uniquely identifies transaction; returns saved result (3)
  // and does not append "BBB".
  r2 = effect(10002ULL, EFFECT_APPEND, "test_reuse_p.txt", "BBB", 3);
  if(r2 != 3){
    printf(1, "FAIL (key reuse returned %d instead of 3)\n", r2);
    exit();
  }

  fd = open("test_reuse_p.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (reopen)\n");
    exit();
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n == 3 && buf[0] == 'A' && buf[1] == 'A' && buf[2] == 'A'){
    printf(1, "PASS\n");
  } else {
    printf(1, "FAIL (len=%d, content=%s)\n", n, buf);
    exit();
  }
}

// Test 3: Key reuse with different operation
void
test_key_reuse_different_op(void)
{
  int r1, r2, fd;

  printf(1, "[TEST 3] Key reuse with different operation... ");

  unlink("test_reuse_op.txt");
  unlink("test_reuse_created.txt");

  fd = open("test_reuse_op.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
  close(fd);

  // First call: APPEND with key 10003
  r1 = effect(10003ULL, EFFECT_APPEND, "test_reuse_op.txt", "DATA", 4);
  if(r1 != 4){
    printf(1, "FAIL (first append returned %d)\n", r1);
    exit();
  }

  // Reusing key 10003 with EFFECT_CREATE on a new path
  // Defined behavior: returns saved result (4), does NOT create test_reuse_created.txt
  r2 = effect(10003ULL, EFFECT_CREATE, "test_reuse_created.txt", 0, 0);
  if(r2 != 4){
    printf(1, "FAIL (key reuse returned %d instead of 4)\n", r2);
    exit();
  }

  // Verify test_reuse_created.txt was NOT created
  fd = open("test_reuse_created.txt", O_RDONLY);
  if(fd >= 0){
    close(fd);
    printf(1, "FAIL (file was created despite key reuse)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// Test 4: Missing file error path and duplicate replay
void
test_missing_file_error(void)
{
  int r1, r2, fd, n;
  char buf[16];

  printf(1, "[TEST 4] Missing file error & idempotency... ");

  unlink("test_missing.txt");

  // First attempt: file does not exist -> fails with -1
  r1 = effect(10004ULL, EFFECT_APPEND, "test_missing.txt", "hello", 5);
  if(r1 != -1){
    printf(1, "FAIL (append to missing file returned %d)\n", r1);
    exit();
  }

  // Now create the file afterwards using normal open()
  fd = open("test_missing.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (open create)\n");
    exit();
  }
  close(fd);

  // Same key must return the original failure (-1) without appending
  r2 = effect(10004ULL, EFFECT_APPEND, "test_missing.txt", "hello", 5);
  if(r2 != -1){
    printf(1, "FAIL (retry returned %d instead of -1)\n", r2);
    exit();
  }

  fd = open("test_missing.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (open read)\n");
    exit();
  }
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n != 0){
    printf(1, "FAIL (file has %d bytes, expected 0)\n", n);
    exit();
  }

  printf(1, "PASS\n");
}

// Test 5: Invalid arguments error path
void
test_invalid_args(void)
{
  int r;

  printf(1, "[TEST 5] Invalid arguments validation... ");

  // Key == 0 is invalid
  r = effect(0ULL, EFFECT_APPEND, "test_seq.txt", "abc", 3);
  if(r != -1){
    printf(1, "FAIL (key 0 accepted)\n");
    exit();
  }

  // Invalid op code
  r = effect(10005ULL, 999, "test_seq.txt", "abc", 3);
  if(r != -1){
    printf(1, "FAIL (invalid op accepted)\n");
    exit();
  }

  // Payload size > BSIZE
  r = effect(10006ULL, EFFECT_APPEND, "test_seq.txt", "abc", 1000);
  if(r != -1){
    printf(1, "FAIL (payload > BSIZE accepted)\n");
    exit();
  }

  // Negative payload size
  r = effect(10007ULL, EFFECT_APPEND, "test_seq.txt", "abc", -1);
  if(r != -1){
    printf(1, "FAIL (negative payload accepted)\n");
    exit();
  }

  printf(1, "PASS\n");
}

// Test 6: CREATE operation and empty payload append
void
test_create_and_empty_payload(void)
{
  int r, fd, n;
  char buf[8];

  printf(1, "[TEST 6] CREATE operation & empty payload... ");

  unlink("test_created.txt");

  // CREATE file via effect()
  r = effect(10008ULL, EFFECT_CREATE, "test_created.txt", 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE returned %d)\n", r);
    exit();
  }

  // Replay CREATE with same key: returns 0
  r = effect(10008ULL, EFFECT_CREATE, "test_created.txt", 0, 0);
  if(r != 0){
    printf(1, "FAIL (CREATE replay returned %d)\n", r);
    exit();
  }

  // Verify file exists
  fd = open("test_created.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (created file not found)\n");
    exit();
  }
  close(fd);

  // Append empty payload (0 bytes)
  r = effect(10009ULL, EFFECT_APPEND, "test_created.txt", "", 0);
  if(r != 0){
    printf(1, "FAIL (empty append returned %d)\n", r);
    exit();
  }

  // Verify file is still empty
  fd = open("test_created.txt", O_RDONLY);
  n = read(fd, buf, sizeof(buf));
  close(fd);
  if(n != 0){
    printf(1, "FAIL (file size is %d, expected 0)\n", n);
    exit();
  }

  printf(1, "PASS\n");
}

// Test 7: CONCURRENCY CHECKPOINT (two concurrent calls sharing same key)
void
test_concurrency_same_key(void)
{
  int pid, fd, n;
  int p[2];
  char buf[16];
  int r_parent, r_child;

  printf(1, "[TEST 7] Concurrency Checkpoint (two processes, same key)... ");

  unlink("test_conc.txt");
  fd = open("test_conc.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
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
    // Child process: call effect() with key 20001
    close(p[0]);
    r_child = effect(20001ULL, EFFECT_APPEND, "test_conc.txt", "X", 1);
    write(p[1], &r_child, sizeof(r_child));
    close(p[1]);
    exit();
  }

  // Parent process: simultaneously call effect() with SAME key 20001
  close(p[1]);
  r_parent = effect(20001ULL, EFFECT_APPEND, "test_conc.txt", "X", 1);

  // Read child's result and wait for child
  r_child = 0;
  read(p[0], &r_child, sizeof(r_child));
  close(p[0]);
  wait();

  if(r_parent != 1 || r_child != 1){
    printf(1, "FAIL (parent_ret=%d child_ret=%d, expected both 1)\n",
           r_parent, r_child);
    exit();
  }

  // Check file contents: MUST be exactly 1 byte ("X"), NOT 2 bytes ("XX")!
  fd = open("test_conc.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (reopen)\n");
    exit();
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n == 1 && buf[0] == 'X'){
    printf(1, "PASS (effect executed exactly once)\n");
  } else {
    printf(1, "FAIL: both executed effect! (file len=%d content=%s)\n", n, buf);
    exit();
  }
}

// Test 8: Multiple concurrent processes sharing same key
void
test_concurrency_multiple(void)
{
  int pids[4];
  int pipes[4][2];
  int i, fd, n;
  char buf[16];
  int r;

  printf(1, "[TEST 8] Multi-process concurrency (4 workers, same key)... ");

  unlink("test_multi.txt");
  fd = open("test_multi.txt", O_CREATE | O_WRONLY);
  if(fd < 0){
    printf(1, "FAIL (create file)\n");
    exit();
  }
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
      r = effect(30001ULL, EFFECT_APPEND, "test_multi.txt", "Z", 1);
      write(pipes[i][1], &r, sizeof(r));
      close(pipes[i][1]);
      exit();
    }
    close(pipes[i][1]);
  }

  // Wait for all 4 children and check their results
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

  // File must contain exactly ONE "Z" (size 1)
  fd = open("test_multi.txt", O_RDONLY);
  if(fd < 0){
    printf(1, "FAIL (reopen)\n");
    exit();
  }
  memset(buf, 0, sizeof(buf));
  n = read(fd, buf, sizeof(buf));
  close(fd);

  if(n == 1 && buf[0] == 'Z'){
    printf(1, "PASS (all 4 received result, effect executed once)\n");
  } else {
    printf(1, "FAIL: multiple workers executed effect! (len=%d content=%s)\n", n, buf);
    exit();
  }
}

int
main(void)
{
  printf(1, "\n=== STARTING PHASE 4 EFFECT TESTS ===\n\n");

  test_sequential_append();
  test_key_reuse_different_payload();
  test_key_reuse_different_op();
  test_missing_file_error();
  test_invalid_args();
  test_create_and_empty_payload();
  test_concurrency_same_key();
  test_concurrency_multiple();

  printf(1, "\n=== ALL PHASE 4 TESTS PASSED! ===\n\n");
  exit();
}
