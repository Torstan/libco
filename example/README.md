# Libco Examples

Build examples from the repository root:

```bash
make examples
```

| Binary | Concept | Arguments | Minimal command |
| --- | --- | --- | --- |
| `example_cond` | Producer/consumer coordination with `CoCond` | none | `./example/example_cond` |
| `example_echosvr` | Coroutine echo server with worker threads | `ip port task_count process_count` | `./example/example_echosvr 0.0.0.0 8080 100 4` |
| `example_echocli` | Coroutine echo client | `ip port connections_per_worker worker_count` | `./example/example_echocli 127.0.0.1 8080 10 100` |
| `example_poll` | Coroutine-aware `poll` over many sockets | `ip port [ip port ...]` | `./example/example_poll 127.0.0.1 8080` |
| `example_setenv` | Coroutine-local environment hook behavior | none | `./example/example_setenv` |
| `example_thread` | Event loops in multiple pthreads | `thread_count` | `./example/example_thread 4` |

For the client and poll examples, start a compatible server first, such as
`example_echosvr`.
