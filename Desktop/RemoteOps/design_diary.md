# Design Diary - RemoteOps (IT24103333)

## Date: October 2026

### Day 1 - Basic Socket Connection
- Created basic Agent and Controller that can connect on port 9410.
- Successfully tested simple TCP connection.

### Day 2 - Multi-threading & Protocol Structure
- Converted Agent to multi-threaded model using pthreads so it can handle multiple Controllers.
- Added read_line(), send_all(), and recv_all() helper functions for correct framing.

### Day 3 - Authentication & Basic Commands
- Implemented AUTH with personalised token OPS-3333.
- Added SID:3333 tagging on every response.
- Implemented SYSINFO, LISTPROC and EXEC (whitelist only).

### Day 4 - File Transfer
- Implemented PUT and GET with exact byte counting.
- Files are stored in ./agentfiles/IT24103333/

### Day 5 - UDP Monitoring & Logging
- Added MONITOR START / STOP with UDP datagrams.
- Added timestamped logging to remoteops_IT24103333.log
- Handled graceful and ungraceful client disconnects.

### Key Decisions
- Chose multi-threading (pthread) instead of select/poll because it is simpler to manage per-client state (authentication + monitoring).
- Used line-based protocol exactly as specified in the assignment.
- Limited EXEC to the fixed whitelist for security.
