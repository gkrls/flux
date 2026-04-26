## DPA Pytorch Backend


### Impementations

**Simple**: calling thread runs D2H + submit inline, then returns. H2D runs later in a completion callback. No deliberate overlap; any concurrency across ops is incidental.

**Pipeline/Latency**: single worker splits one tensor into chunks and overlaps D2H, allreduce, and H2D of different chunks on separate CUDA streams. Overlap is within one op.

**Worksteal/Throughput**: calling thread enqueues and returns; a pool worker picks up the op and runs it end-to-end. Multiple ops in flight on different workers, so D2H of op N+1 overlaps with allreduce of op N and H2D of op N-1. Overlap is across ops
Note exactly workstealing. Only in the sense that the thread creating the op is not the one to run/finish it.