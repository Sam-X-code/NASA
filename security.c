#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "security.h"

static struct spinlock security_lock;
static struct sleeplock security_gate;
static struct agent_context boot_context;
static int next_session_id = 1;

void
security_init(void)
{
  initlock(&security_lock, "security");
  initsleeplock(&security_gate, "security gate");
  acquire(&security_lock);
  boot_context.session_id = next_session_id++;
  boot_context.tainted = 0;
  release(&security_lock);
}

void
security_enter(void)
{
  acquiresleep(&security_gate);
}

void
security_exit(void)
{
  releasesleep(&security_gate);
}

struct agent_context *
security_context(void)
{
  return &boot_context;
}

int
security_context_available(void)
{
  struct proc *p = myproc();

  return p != 0 && p->agent_ctx != 0;
}

int
security_is_tainted(void)
{
  struct proc *p = myproc();
  int value;

  if(p == 0 || p->agent_ctx == 0)
    return 1;

  acquire(&security_lock);
  value = p->agent_ctx->tainted;
  release(&security_lock);
  return value;
}

int
security_mark_tainted(void)
{
  struct proc *p = myproc();

  if(p == 0 || p->agent_ctx == 0)
    return -1;

  acquire(&security_lock);
  p->agent_ctx->tainted = 1;
  release(&security_lock);
  return 0;
}
