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

void
effect_init(void)
{
  initsleeplock(&effect_lock, "effect");
  readsb(ROOTDEV, &effect_sb);

  if(effect_sb.effblocks == 0)
    panic("effect: no table blocks");
}

// Returns 0 if found, -1 if not found.
int
effect_lookup(unsigned long long key, unsigned long long *result)
{
  uint capacity, start, i, index, blockno, slot;
  struct buf *bp;
  struct effect_entry *entries;

  if(key == 0)
    return -1;

  acquiresleep(&effect_lock);

  capacity = effect_sb.effblocks * EFFECT_ENTRIES_PER_BLOCK;
  start = (uint)key % capacity;

  for(i = 0; i < capacity; i++){
    index = (start + i) % capacity;
    blockno = effect_sb.effstart +
              index / EFFECT_ENTRIES_PER_BLOCK;
    slot = index % EFFECT_ENTRIES_PER_BLOCK;

    bp = bread(ROOTDEV, blockno);
    entries = (struct effect_entry *)bp->data;

    if(entries[slot].key == key){
      *result = entries[slot].result;
      brelse(bp);
      releasesleep(&effect_lock);
      return 0;
    }

    if(entries[slot].key == 0){
      brelse(bp);
      releasesleep(&effect_lock);
      return -1;
    }

    brelse(bp);
  }

  releasesleep(&effect_lock);
  return -1;
}

// Returns 0 if inserted, 1 if key already exists,
// -1 if the table is full or key is invalid.
int
effect_insert(unsigned long long key, unsigned long long result)
{
  uint capacity, start, i, index, blockno, slot;
  struct buf *bp;
  struct effect_entry *entries;

  if(key == 0)
    return -1;

  acquiresleep(&effect_lock);
  begin_op();

  capacity = effect_sb.effblocks * EFFECT_ENTRIES_PER_BLOCK;
  start = (uint)key % capacity;

  for(i = 0; i < capacity; i++){
    index = (start + i) % capacity;
    blockno = effect_sb.effstart +
              index / EFFECT_ENTRIES_PER_BLOCK;
    slot = index % EFFECT_ENTRIES_PER_BLOCK;

    bp = bread(ROOTDEV, blockno);
    entries = (struct effect_entry *)bp->data;

    if(entries[slot].key == key){
      brelse(bp);
      end_op();
      releasesleep(&effect_lock);
      return 1;
    }

    if(entries[slot].key == 0){
      entries[slot].key = key;
      entries[slot].result = result;

      log_write(bp);
      brelse(bp);

      end_op();
      releasesleep(&effect_lock);
      return 0;
    }

    brelse(bp);
  }

  end_op();
  releasesleep(&effect_lock);
  return -1;
}