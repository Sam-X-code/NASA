#define EFFECT_APPEND 1
#define EFFECT_CREATE 2

// Crash injection points for crashat(n)
// 0: disabled
#define CRASH_OFF           0

// Named commit-phase crash points (101-105)
#define CRASH_COMMIT_BEFORE 101 // inside commit(), before write_head() (uncommitted)
#define CRASH_COMMIT_AFTER  102 // inside commit(), after write_head() (committed!)
#define CRASH_INSTALL_MID   103 // inside install_trans(), while writing home blocks
#define CRASH_CLEAR_BEFORE  104 // inside commit(), after install_trans, before log clear
#define CRASH_CLEAR_AFTER   105 // inside commit(), after log clear write_head()
