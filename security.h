struct agent_context {
  int session_id;
  int tainted;
};

struct agent_context *security_context(void);
void security_enter(void);
void security_exit(void);
