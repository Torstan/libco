# Libco Examples

Build examples from the repository root:

```bash
make examples
```

| Binary | Concept | Arguments | Minimal command |
| --- | --- | --- | --- |
| `example_cond` | Producer/consumer coordination with `CoCond` | none | `./example/example_cond` |
| `example_echosvr` | Coroutine echo server with worker threads | `ip port workers` | `./example/example_echosvr 0.0.0.0 8080 4` |
| `example_echocli` | Coroutine echo client | `ip port connections loops` | `./example/example_echocli 127.0.0.1 8080 10 100` |
| `example_poll` | Coroutine-aware `poll` over many sockets | `ip port coroutine_count fd_count` | `./example/example_poll 127.0.0.1 8080 10 100` |
| `example_setenv` | Coroutine-local environment hook behavior | none | `./example/example_setenv` |
| `example_thread` | Event loops in multiple pthreads | `thread_count` | `./example/example_thread 4` |

For the client and poll examples, start a compatible server first, such as
`example_echosvr`.
