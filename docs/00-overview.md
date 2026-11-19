# Redis Clone in C++ — Overview

> **Source we follow:** *Build Your Own Redis with C/C++* by James Smith — https://build-your-own.org/redis/ (free to read online).
> Every step below maps to one chapter of that book. We write the code ourselves in C++ and read the chapter for reference.

---

## 1. What is Redis?

**Redis** is a program that stores data in **RAM** (main memory) and answers questions about that data over the **network**.

The data is stored as **key → value** pairs. A key is a name. A value is the data saved under that name.

```
key           value
------------  -----------------------------
"user:42"     "Charudev"
"views"       "1093"
"leaderboard" { (bob, 250), (alice, 310) }   <- a sorted set
```

### Why does Redis exist?

Normal databases (Postgres, MySQL) store data on **disk**. Redis stores data in **RAM**. RAM is much faster to read.

| Where the data lives | Time to read 1 item | Compared to RAM |
|---|---|---|
| RAM | ~100 nanoseconds | 1x |
| SSD (disk) | ~100 microseconds | ~1,000x slower |
| Postgres query over the network | ~1–10 milliseconds | ~10,000x+ slower |

So companies put Redis **in front of** their main database. The app asks Redis first. If the data is there (a "cache hit"), the app skips the slow database.

```
  App ──GET user:42──► Redis ──(hit)──► returns in ~0.2 ms
                         │
                       (miss)
                         ▼
                      Postgres ──► returns in ~5 ms, app saves a copy in Redis
```

> [!NOTE]
> You already used Redis in your KYC internship: "Redis-queued retries" means Redis held the list of webhooks waiting to be retried.

---

## 2. What we are building (the aim)

Two programs:

| Program | What it does |
|---|---|
| `server` | Listens on TCP port **1234**. Holds all the data in RAM. Serves many clients at the same time on **one thread**. |
| `client` | Connects to the server, sends one command, prints the reply. Used for testing. |

### Commands the finished server will support

| Command | Example | Meaning |
|---|---|---|
| `set` | `set name chard` | Save value `chard` under key `name` |
| `get` | `get name` | Return the value of `name` |
| `del` | `del name` | Delete the key |
| `keys` | `keys` | List every key |
| `zadd` | `zadd board 310 alice` | Add `alice` with score 310 to sorted set `board` |
| `zscore` | `zscore board alice` | Return alice's score |
| `zrem` | `zrem board alice` | Remove alice from the sorted set |
| `zquery` | `zquery board 200 "" 0 10` | Return up to 10 items with score ≥ 200, in order |
| `pexpire` | `pexpire name 5000` | Delete `name` automatically after 5000 ms |
| `pttl` | `pttl name` | How many ms are left before `name` expires |

### The 5 resume bullets → what proves each one

| Resume claim | Where it lives in our code |
|---|---|
| In-memory KV server over TCP sockets | `server.cpp`, `client.cpp` |
| Non-blocking poll-based event loop | the `while (true)` loop in `server.cpp` |
| Hashtable for O(1) key lookups | `hashtable.h/.cpp` |
| AVL tree for sorted-set operations | `avl.h/.cpp`, `zset.h/.cpp` |
| TTL with a min-heap + thread pool | `heap.h/.cpp`, `thread_pool.h/.cpp` |

> [!IMPORTANT]
> Your resume says **"in C"**. We are writing **C++**. Change that word on the resume to **C++** so it matches the repo.

---

## 3. First principles — the 8 ideas you need

Read these in order. Each idea is used by the one after it.

### 3.1 Process and file descriptor

A **process** is a running program. The operating system (Linux) gives the process small integers called **file descriptors (fd)** to refer to open things: files and network connections.

```
fd 0 = keyboard input    fd 1 = screen output    fd 2 = error output
fd 3 = listening socket  fd 4 = client A         fd 5 = client B ...
```

We call `read(fd, ...)` and `write(fd, ...)` on these numbers. The same two functions work for files and for network connections.

### 3.2 Socket and TCP

A **socket** is a file descriptor that sends and receives bytes over the network.

**TCP** is the set of rules that delivers those bytes **in order** and **without loss**. TCP has one property that matters a lot for us:

> [!WARNING]
> TCP is a **byte stream**. It does **not** keep message boundaries.
> If the client sends `"hello"` then `"world"`, the server may receive `"hel"` then `"loworld"`. One `read()` is not one message.

How a connection is made (system calls, in order):

```
server:  socket() → bind(port 1234) → listen() → accept() ──► new fd per client
client:  socket() → connect(127.0.0.1:1234) ───────────────┘
```

### 3.3 Protocol (how we find message boundaries)

TCP has no boundaries, so **we** add them. Every message starts with a **4-byte length**, then that many bytes of data.

```
┌───────────────┬──────────────────────┐
│ len (4 bytes) │ data (len bytes)     │
└───────────────┴──────────────────────┘
  05 00 00 00     h  e  l  l  o
```

A request (a command) is a list of strings. It is encoded as: the number of strings, then each string with its own length.

```
"set name chard"  →  [nstr=3] [len=3]"set" [len=4]"name" [len=5]"chard"
```

A reply starts with a **1-byte type tag**, so the client knows how to read the rest:

| Tag | Type | Example |
|---|---|---|
| 0 | nil (nothing) | `get` on a missing key |
| 1 | error | unknown command |
| 2 | string | `"chard"` |
| 3 | integer | `1` |
| 4 | double | `310.0` |
| 5 | array | reply of `keys` |

### 3.4 Blocking vs non-blocking

A **blocking** `read()` stops the whole program until data arrives. With 2 clients, if client A is slow, client B waits too.

A **non-blocking** `read()` returns at once. If no data has arrived yet, it returns -1 with the error code `EAGAIN` ("try again later"). We turn this mode on with `fcntl(fd, F_SETFL, O_NONBLOCK)`.

### 3.5 Event loop with `poll()`

Three ways to serve many clients:

| Model | How | Memory per client | Used by |
|---|---|---|---|
| 1 process per client | `fork()` | ~MBs | old Apache |
| 1 thread per client | `pthread_create()` | ~64 KB–8 MB stack | many Java servers |
| **1 thread + event loop** | `poll()` | ~hundreds of bytes | **Redis**, Nginx, Node.js |

`poll()` takes a list of fds and **waits until at least one is ready** (has data to read, or has room to write). We handle only the ready ones, then call `poll()` again.

```
          ┌────────────────────────────────────────────────┐
          ▼                                                │
  build fd list (listening fd + every client fd)           │
          │                                                │
  poll(list, timeout)    ◄── waits here, uses 0% CPU       │
          │                                                │
  listening fd ready?        → accept() the new client     │
  client fd ready to read?   → read(), parse, run command  │
  client fd ready to write?  → write() the reply           │
  timers due?                → close idle clients,         │
                               delete expired keys         │
          └────────────────────────────────────────────────┘
```

Only one thread touches the data, so the data needs **no locks**. This is why Redis is single-threaded.

### 3.6 Hashtable

A **hashtable** stores keys in an array of slots. A **hash function** turns the key string into a number. `number % array_size` picks the slot. Finding a key costs 1 hash + 1 array index: **O(1)** on average.

Two keys can land in the same slot (a **collision**). Each slot keeps a **linked list** of its keys ("chaining").

```
slots:  [0] → ("views", 1093) → ("x", 7)
        [1] → null
        [2] → ("user:42", "Charudev")
        [3] → null
```

When the lists get long, we need a bigger array, and every key must move to it. Moving 10 million keys at once would stop the server for about 1 second. So we use **progressive rehashing**: keep the old and the new table side by side, and move **128 keys per request** until the old table is empty.

### 3.7 AVL tree (for the sorted set)

A **sorted set** keeps items ordered by score. It needs two fast operations:
1. Find an item by name → a **hashtable** (name → node).
2. Walk items in score order, and find "the first item with score ≥ X" → a **balanced binary search tree**.

A plain binary search tree can become a straight line (for example when items arrive already sorted). Then search costs O(n). An **AVL tree** checks after every insert and delete that, at every node, the left and right subtree heights differ by **at most 1**. If not, it fixes the shape with **rotations**. Height stays below ~1.44 × log₂(n), so search, insert and delete are all **O(log n)**.

```
 unbalanced (height 3)       after a left rotation (height 2)
    1                                2
     \                              / \
      2                            1   3
       \
        3
```

Each node also stores the **count** of nodes in its subtree. With it, `zquery ... offset` skips `offset` items in O(log n) instead of walking past them one at a time.

### 3.8 Min-heap (for TTL) and thread pool

**TTL (time to live)** means a key deletes itself after N ms. The server must quickly answer: "which key expires **next**?"

A **min-heap** is an array arranged as a binary tree in which every parent is ≤ its children. The smallest value (the earliest expiry time) is always at index 0. Reading it is O(1). Adding or removing a value is O(log n).

```
 array:  [ 100, 300, 200, 900, 400 ]    (expiry times in ms)
 tree:          100
              /     \
            300     200
           /   \
         900   400
```

The event loop uses that smallest time to set the `poll()` timeout, so it wakes up exactly when the next key must expire.

A **thread pool** is a fixed number of worker threads (we use 4) that wait on a queue of jobs. The main thread must never do slow work, because every client waits while it does. Freeing a sorted set with 1,000,000 items takes many milliseconds, so the main thread puts that job on the queue and a worker does it.

---

## 4. High-level design

```
                       ┌────────────────────────── server process ──────────────────────────┐
 client ─TCP:1234─►    │                                                                    │
 client ─TCP:1234─►    │  ┌─────────────┐  bytes   ┌────────────┐  cmd   ┌──────────────┐   │
 client ─TCP:1234─►    │  │ Event loop  │ ───────► │ Parser +   │ ─────► │ Dispatcher   │   │
                       │  │  (poll)     │ ◄─────── │ Serializer │ ◄───── │ get/set/z*   │   │
                       │  └──────┬──────┘  reply   └────────────┘ result └──────┬───────┘   │
                       │         │                                              │           │
                       │         │ timers                                       ▼           │
                       │  ┌──────┴────────┐                     ┌────────────────────────┐  │
                       │  │ idle list     │                     │ Keyspace (hashtable)   │  │
                       │  │ TTL min-heap  │ ───── expire ─────► │   key → Entry          │  │
                       │  └───────────────┘                     │   Entry = string       │  │
                       │                                        │         or ZSet        │  │
                       │  ┌───────────────┐   free big ZSet     │   ZSet = hashtable     │  │
                       │  │ Thread pool   │ ◄────────────────── │         + AVL tree     │  │
                       │  │ (4 workers)   │                     └────────────────────────┘  │
                       │  └───────────────┘                                                 │
                       └────────────────────────────────────────────────────────────────────┘
```

| Component | Job | Talks to |
|---|---|---|
| Event loop | Waits on all sockets, reads and writes bytes | Parser, Timers |
| Parser / Serializer | Bytes → command; result → reply bytes | Event loop, Dispatcher |
| Dispatcher | Picks the function for `get`, `zadd`, ... | Keyspace |
| Keyspace | Stores every key and its value | Dispatcher, Timers |
| Timers | Close idle clients, delete expired keys | Event loop, Keyspace |
| Thread pool | Frees large values off the main thread | Keyspace |

### Life of one request: `set name chard`

```
1. client writes the encoded request to its socket
2. poll() reports: the client fd is readable
3. read() appends the bytes to conn->incoming
4. parser: is a full message present (4-byte len + whole body)?
      no  → go back to poll() and wait for more bytes
      yes ↓
5. parse body → ["set", "name", "chard"]
6. dispatcher → do_set() → hashtable lookup "name" → insert, or overwrite
7. serializer appends the reply [tag 0 = nil] to conn->outgoing
8. remove the parsed message from the front of conn->incoming
9. poll() reports: the client fd is writable → write() conn->outgoing
```

---

## 5. Low-level design

### 5.1 Files

```
redis-clone/
├── Makefile
├── src/
│   ├── common.h            container_of macro, string hash
│   ├── server.cpp          event loop, protocol, commands
│   ├── client.cpp          test client
│   ├── hashtable.h/.cpp    HMap with progressive rehashing
│   ├── avl.h/.cpp          AVL tree
│   ├── zset.h/.cpp         sorted set = HMap + AVL tree
│   ├── heap.h/.cpp         min-heap for TTL
│   ├── list.h              doubly linked list for idle timers
│   └── thread_pool.h/.cpp
├── tests/                  unit tests + a command test script
└── docs/                   these notes, one file per step
```

### 5.2 Key structs

```cpp
// One client connection
struct Conn {
    int fd = -1;
    bool want_read = false;         // ask poll() to report "readable"
    bool want_write = false;        // ask poll() to report "writable"
    bool want_close = false;        // close at the end of this loop iteration
    std::vector<uint8_t> incoming;  // bytes read, not parsed yet
    std::vector<uint8_t> outgoing;  // reply bytes, not written yet
    uint64_t last_active_ms = 0;
    DList idle_node;                // position in the idle-timer list
};

// Hashtable node. It holds no data. It is placed INSIDE the data struct.
struct HNode {
    HNode *next = nullptr;
    uint64_t hcode = 0;             // cached hash of the key
};

// One key in the keyspace
struct Entry {
    HNode node;                     // links this Entry into the hashtable
    std::string key;
    uint32_t type = 0;              // T_STR or T_ZSET
    std::string str;
    ZSet zset;
    size_t heap_idx = -1;           // index in the TTL heap (-1 = no TTL)
};

struct AVLNode {
    AVLNode *parent = nullptr, *left = nullptr, *right = nullptr;
    uint32_t height = 1;            // used for balancing
    uint32_t cnt = 1;               // nodes in this subtree, for offset queries
};
```

### 5.3 The idea used everywhere: intrusive data structures

> [!TIP]
> This is the design choice interviewers find most interesting. Learn to explain it.

In a normal C++ container (`std::unordered_map<std::string, Entry>`), the container allocates its own node and stores our data inside it.

In an **intrusive** structure it is the other way round. The link fields (`HNode`, `AVLNode`, `DList`) are placed **inside** our own struct. The hashtable only sees `HNode*`. To get the full `Entry` back, we subtract the field's offset from the pointer:

```cpp
#define container_of(ptr, T, member) \
    ((T *)((char *)(ptr) - offsetof(T, member)))

Entry *e = container_of(hnode_ptr, Entry, node);
```

| Benefit | Why it happens |
|---|---|
| One memory allocation per item, not two | The link lives inside the item |
| One item can be in several structures | A ZSet node is in the hashtable **and** the AVL tree, with no copy |
| One generic hashtable for every use | The keyspace and every ZSet share `hashtable.cpp` |

### 5.4 Timers

| Timer | All timeouts equal? | Structure | Why |
|---|---|---|---|
| Idle connection (5 s) | Yes | Doubly linked list | An active conn moves to the tail, so the head is always the oldest. Every operation is O(1) |
| Key TTL | No, each key differs | Min-heap | Insert times arrive in any order, and we need the smallest one fast |

On each loop: `poll timeout = min(next idle deadline, heap[0].time) − now`.

---

## 6. Build plan

Each step ends with something you can **run and test**. Each step is one commit, so the git history shows how the project grew.

| # | Step | Book chapter | Test | Hours |
|---|---|---|---|---|
| 1 | TCP server + client say hello | Ch 2–3 | client prints the reply | 1.5 |
| 2 | Length-prefixed protocol | Ch 4 | 3 messages → 3 replies | 1.5 |
| 3 | Non-blocking `poll()` event loop | Ch 5–6 | 2 clients, neither waits | 3 |
| 4 | get / set / del on `std::map` | Ch 7 | `set a 1` then `get a` | 1.5 |
| 5 | Own hashtable, progressive rehash | Ch 8 | 100k keys insert + lookup | 3 |
| 6 | Typed replies + `keys` | Ch 9 | `keys` returns an array | 1.5 |
| 7 | AVL tree | Ch 10 | random ops keep balance | 3 |
| 8 | Sorted set commands | Ch 11 | leaderboard script | 3 |
| 9 | Idle-connection timeout | Ch 12 | silent client dropped at 5 s | 1.5 |
| 10 | TTL: pexpire / pttl | Ch 13 | key gone after N ms | 2 |
| 11 | Thread pool for big frees | Ch 14 | 1M-item delete, no stall | 2 |
| 12 | README, benchmark, cleanup | — | `make test` passes | 2 |
| | **Total** | | | **~26** |

### How each step works between us

```
 read the plain-language note for the step (docs/NN-*.md)
      ↓
 you write the code (I give the skeleton + hints; full code only if you are stuck)
      ↓
 build and run it in WSL:
     wsl -d Ubuntu-24.04
     cd /mnt/c/Users/chard/Desktop/WebDevelopment/redis-clone
     make && ./server
      ↓
 I review it → you commit
```

> [!NOTE]
> We compile in **WSL Ubuntu 24.04**. g++ 13.3 and make 4.3 are already installed. `poll()`, `fcntl()`, `<sys/socket.h>` and pthreads are Linux APIs. They do not exist in plain Windows.

---

## 7. Interview questions to be ready for by the end

| Question | Short answer to expand on |
|---|---|
| Why single-threaded? | No locks on data; the limit is usually network and memory, not CPU |
| Why `poll()`, not a thread per client? | Thousands of clients at ~bytes each, not ~MBs each |
| What does `poll()` cost with 10k clients? | O(n) per call; `epoll` is O(ready fds), so switch at scale |
| How do you know a message is complete? | 4-byte length prefix, because TCP is a byte stream |
| Why progressive rehashing? | Resize in small steps, so no single request stalls |
| Why AVL, not a skip list like real Redis? | Both O(log n); AVL is deterministic; `cnt` gives rank queries |
| Why a heap for TTL but a list for idle timeouts? | Idle timeouts are all equal, so FIFO order = time order |
| What goes to the thread pool? | Freeing huge containers: slow work that no client needs to wait for |
| What if the server restarts? | All data is lost; real Redis adds RDB snapshots and AOF logs |
