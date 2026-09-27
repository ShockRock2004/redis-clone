# redis-clone

An in-memory key-value server written in C that speaks a plain-text protocol over TCP.

- Non-blocking, single-threaded event loop built on `poll()`
- Chained hashtable for O(1) key lookups
- Sorted sets backed by an AVL tree, plus a member-to-score index
- TTL key expiry through a min-heap of deadlines
- Thread pool that frees large values off the event loop

## Build & run (Linux / WSL / macOS)

```sh
make
./redis-clone 6380
```

## Usage

```
$ nc localhost 6380
SET name alice
OK
EXPIRE name 10
1
TTL name
10
ZADD board 42 bob
1
ZADD board 17 carol
1
ZRANGEBYSCORE board 0 10
carol 17
bob 42
END
```

Commands: `PING`, `SET k v`, `GET k`, `DEL k`, `KEYS`, `EXPIRE k secs`, `TTL k`, `PERSIST k`,
`ZADD k score member`, `ZREM k member`, `ZSCORE k member`, `ZRANGEBYSCORE k min limit`.
