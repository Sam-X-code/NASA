#include "types.h"
#include "user.h"

int
main(void)
{
  printf(1, "untrusted executable ran\n");
  exit();
}
