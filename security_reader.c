#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  char buf[16];
  int fd, n;

  fd = open("inbox.txt", 0);
  if(fd < 0)
    exit();
  n = read(fd, buf, sizeof(buf));
  close(fd);
  if(n > 0)
    printf(1, "security_reader: read untrusted data\n");
  exit();
}
