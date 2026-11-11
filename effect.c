#include "types.h"
#include "x86.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

static struct superblock effect_sb;
static struct sleeplock effect_lock;


#define EFFECT_ENTRIES_PER_BLOCK (BSIZE / sizeof(struct effect_entry))

static uint
effect_capacity(void)
{
  return effect_sb.effblocks * EFFECT_ENTRIES_PER_BLOCK;
}

static int 
effect_find(unsigned long long key, uint *free_index, unsigned long long * result)
{
  uint capacity, start, i, index,blockno, slot;
  struct buf *bp;
  struct effect_entry *entries;

  if(key == 0)
    return -1;

  capacity = effect_capacity();

  if(capacity == 0) return -1;

  start = (uint)key % capacity;

  for(i = 0; i < capacity; i++){
    index = (start + i) % capacity;
    blockno = effect_sb.effstart + index / EFFECT_ENTRIES_PER_BLOCK;
    slot = index % EFFECT_ENTRIES_PER_BLOCK;

    bp = bread(ROOTDEV, blockno);
    entries = (struct effect_entry *)bp->data;

    if(entries[slot].key == key){
      if(result)
        *result = entries[slot].result;
      brelse(bp);
      return 0;
    }

    if(entries[slot].key == 0){
      if(free_index)
        *free_index = index;
      brelse(bp);
      return 1;
    }

    brelse(bp);
  }

  return -1;
}

void
effect_init(void)
{
  initsleeplock(&effect_lock, "effect");
  readsb(ROOTDEV, &effect_sb);

  if(effect_sb.effblocks == 0)
    panic("effect: no table blocks");
}

// Returns 0 if found, -1 if not found.

void effect_tx_begin(void)
{
  acquiresleep(&effect_lock);
  begin_op();
}

void effect_tx_end(void)
{
  end_op();
  releasesleep(&effect_lock);
} 

int effect_tx_lookup(unsigned long long key, unsigned long long * result)
{
  return effect_find(key, 0, result)==0? 0:-1;
}


int effect_tx_has_space(unsigned long long key)
{
  uint free_index; int status;
  status = effect_find(key, &free_index, 0);
  return status == 1 ? 0:-1;
}

int effect_tx_insert(unsigned long long key, unsigned long long result)
{
  uint free_index; int status;
  struct buf *bp;
  struct effect_entry *entries;
  uint blockno, slot;

  if(key==0)
    return -1;

  status = effect_find(key, &free_index, 0);
  if(status != 1)
    return -1;

  blockno = effect_sb.effstart + free_index / EFFECT_ENTRIES_PER_BLOCK;
  slot = free_index % EFFECT_ENTRIES_PER_BLOCK;

  bp = bread(ROOTDEV, blockno);
  entries = (struct effect_entry *)bp->data;

  if(entries[slot].key != 0){
    brelse(bp);
    return -1;
  }

  entries[slot].key = key;
  entries[slot].result = result;

  log_write(bp);
  brelse(bp);

  return 0;
}

int
effect_lookup(unsigned long long key, unsigned long long *result)
{
  int found;
  if(key==0)
    return -1;
  acquiresleep(&effect_lock);
  found = effect_find(key, 0, result) ==0? 0:-1;
  releasesleep(&effect_lock);
  return found;
}

// Returns 0 if inserted, 1 if key already exists,
// -1 if the table is full or key is invalid.
int
effect_insert(unsigned long long key, unsigned long long result)
{
  int status;
  if(key==0)
    return -1;
  effect_tx_begin();
  
  status = effect_tx_lookup(key, 0);
  if(status == 0){
    effect_tx_end();
    return 1;
  }
  

  status = effect_tx_insert(key, result);

  effect_tx_end();
  return status;
}