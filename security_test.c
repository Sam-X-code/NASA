#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"

static int failures;

static void
check(char *name, int passed)
{
  printf(1, "%s: %s\n", name, passed ? "PASS" : "FAIL");
  if(!passed)
    failures++;
}

static int
create_probe(void)
{
  int fd;

  fd = open("probe.txt", O_CREATE | O_RDWR);
  if(fd < 0)
    return -1;
  if(write(fd, "probe", 5) != 5){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
trust_is(char *path, short trust)
{
  struct stat st;

  return stat(path, &st) == 0 && st.type == T_FILE && st.trust == trust;
}

static void
check_exec_fixture_metadata(void)
{
  check("init is trusted", trust_is("init", TRUSTED));
  check("shell is trusted", trust_is("sh", TRUSTED));
  check("security test driver is trusted",
        trust_is("security_test", TRUSTED));
  check("trusted executable fixture is trusted",
        trust_is("trusted_exec", TRUSTED));
  check("unlink probe is trusted",
        trust_is("exec_probe.txt", TRUSTED));
  check("untrusted executable fixture is untrusted",
        trust_is("untrusted_exec", UNTRUSTED));
}

static void
run_exec_test(char *mode)
{
  char *trusted_args[] = { "trusted_exec", 0 };
  char *untrusted_args[] = { "untrusted_exec", 0 };
  char **args;
  char *path;
  int pid, unlink_result;

  if(strcmp(mode, "trusted") == 0){
    path = "trusted_exec";
    args = trusted_args;
  } else {
    path = "untrusted_exec";
    args = untrusted_args;
  }

  pid = fork();
  if(pid == 0){
    exec(path, args);
    printf(1, "exec fixture failed: %s\n", path);
    exit();
  }
  if(pid < 0){
    check("fixture forked and exec'd", 0);
    return;
  }
  wait();

  unlink_result = unlink("exec_probe.txt");
  if(strcmp(mode, "trusted") == 0)
    check("trusted execution leaves context untainted",
          unlink_result == 0);
  else
    check("untrusted execution taints shared context",
          unlink_result < 0);
}

#ifdef EXEC_TRUST_TEST
static void
run_missing_context_test(void)
{
  int marker = 0x236;
  int result;

  result = exec_no_context_test("untrusted_exec");
  check("missing-context exec rejected without replacing address space",
        result == 0 && marker == 0x236);
  check("rejected exec does not taint context",
        unlink("exec_probe.txt") == 0);
}
#endif

int
main(int argc, char **argv)
{
  char buf[32];
  int fd, n, pid;
  struct stat st, st2;
  char *args[] = { "security_reader", 0 };

  if(argc == 2){
    check_exec_fixture_metadata();
    if(strcmp(argv[1], "trusted") == 0 ||
       strcmp(argv[1], "untrusted") == 0){
      run_exec_test(argv[1]);
    } else if(strcmp(argv[1], "missing-context") == 0){
#ifdef EXEC_TRUST_TEST
      run_missing_context_test();
#else
      check("missing-context test requires EXEC_TRUST_TEST", 0);
#endif
    } else {
      check("valid executable test mode supplied", 0);
    }
    printf(1, "security_test: %d failure(s)\n", failures);
    exit();
  }

    check("fresh boot starts untainted",
        create_probe() == 0 && unlink("probe.txt") == 0);
    check("hard link allowed before taint",
        link("report.txt", "report_link.txt") == 0 &&
        stat("report.txt", &st) == 0 &&
        stat("report_link.txt", &st2) == 0 &&
        st.ino == st2.ino &&
        unlink("report_link.txt") == 0);

  check("secdata starts trusted",
        stat("secdata.txt", &st) == 0 && st.trust == TRUSTED);
  fd = open("report.txt", 0);
  n = fd < 0 ? -1 : read(fd, buf, sizeof(buf));
  if(fd >= 0)
    close(fd);
  check("trusted read succeeds without taint",
        n > 0 && create_probe() == 0 && unlink("probe.txt") == 0);

  fd = open("inbox.txt", 0);
  n = fd < 0 ? -1 : read(fd, buf, 0);
  if(fd >= 0)
    close(fd);
  check("zero-byte untrusted read does not taint",
        n == 0 && create_probe() == 0 && unlink("probe.txt") == 0);

  check("failed read does not taint",
        read(-1, buf, sizeof(buf)) < 0 &&
        create_probe() == 0 && unlink("probe.txt") == 0);

  check("taint fixture remains trusted before propagation",
        stat("secdata.txt", &st) == 0 && st.trust == TRUSTED);
  check("create post-taint unlink probe", create_probe() == 0);

  pid = fork();
  if(pid == 0){
    exec("security_reader", args);
    exit();
  }
  if(pid > 0)
    wait();
  check("fork/exec child read untrusted file",
        pid > 0 && unlink("probe.txt") < 0);
  check("hard link blocked after taint",
        link("important.txt", "important_link.txt") < 0 &&
        stat("important.txt", &st) == 0 &&
        stat("important_link.txt", &st2) < 0);
  check("tainted write downgrades trusted destination",
        (fd = open("secdata.txt", O_WRONLY)) >= 0 &&
        write(fd, "tainted\n", 8) == 8 &&
        fstat(fd, &st) == 0 && st.trust == UNTRUSTED);
  if(fd >= 0)
    close(fd);

  check("tainted process cannot unlink important.txt",
        unlink("important.txt") < 0 && stat("important.txt", &st) == 0);

  printf(1, "security_test: %d failure(s)\n", failures);
  exit();
}
