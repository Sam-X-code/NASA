#include "types.h"
#include "user.h"
#include "effect.h"

#define UNTRUSTED_CREATE_KEY 0xA1300001ULL
#define UNTRUSTED_APPEND_KEY 0xA1300002ULL

int
main(void)
{
  int create_result, append_result;

  create_result = effect(UNTRUSTED_CREATE_KEY, EFFECT_CREATE,
                         "/agentlog.txt", 0, 0);
  append_result = effect(UNTRUSTED_APPEND_KEY, EFFECT_APPEND,
                         "/important.txt", "X", 1);
  if(create_result != -1 || append_result != -1){
    printf(1, "UNTRUSTED_EFFECT_AUTH_TEST: FAIL create=%d append=%d\n",
           create_result, append_result);
    exit();
  }
  printf(1, "untrusted direct create denied: PASS\n");
  printf(1, "untrusted direct append denied: PASS\n");
  printf(1, "UNTRUSTED_EFFECT_AUTH_TEST: PASS\n");
  exit();
}
