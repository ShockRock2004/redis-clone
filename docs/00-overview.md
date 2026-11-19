# Redis Clone in C++ — Explained From First Principles

> **Source we follow:** *Build Your Own Redis with C/C++* by James Smith — https://build-your-own.org/redis/ (free online).
> We follow its chapter order. We write our own code in modern C++: classes, `std::vector`, `std::string`, `std::thread`, smart pointers.

**How to read this file:** in order, top to bottom. Every part uses only ideas from the parts above it. Each part says **What** it is, **Why** we need it, and **Where it fits** in the project.

```
Part 1  Computer basics      → bytes, RAM, programs, the operating system
Part 2  Networking           → client, server, IP, port, TCP, socket
Part 3  Our message format   → how the server knows where a message ends
Part 4  Many clients at once → non-blocking mode, poll(), the event loop
Part 5  Storing keys         → the hashtable
Part 6  Sorted sets          → the AVL tree
Part 7  Time                 → idle timeouts, key expiry, the min-heap
Part 8  Slow work            → the thread pool
Part 9  The full design      → every C++ class and how they connect
Part 10 Build plan           → 12 steps
Part 11 Interview: "your resume says C"
```

---

# Part 0 — The goal in one paragraph

We will write **one C++ program called `server`**. It runs on your computer. It keeps a list of **names and values** in memory, for example `name → "chard"`. Other programs (called **clients**) connect to it over the network and send short text commands such as `set name chard` or `get name`. The server answers each command. It must answer **many clients at the same time**, it must answer **fast** (well under 1 millisecond per command), and it must do this using **one main thread**. This is a small copy of a real product called **Redis**.

We will also write a second small program called `client`, only for testing the server.

---

# Part 1 — Computer basics

## 1.1 Bytes

**What:** A **byte** is a number from 0 to 255. Every piece of data in a computer is a list of bytes.

Text is stored by giving each letter a number (this numbering is called **ASCII**):

```
text:   h    e    l    l    o
bytes:  104  101  108  108  111      ← "hello" is 5 bytes
```

A bigger number needs more bytes. A **4-byte number** (`uint32_t` in C++) can hold 0 to 4,294,967,295. On Intel/AMD CPUs, the 4 bytes are stored **lowest byte first** ("little-endian"):

```
the number 16   → bytes 16  0  0  0
the number 300  → bytes 44  1  0  0      (because 300 = 1×256 + 44)
```

**Why:** The network only carries bytes. Every command and every answer must be turned into bytes and back.
**Where it fits:** Part 3 (our message format) is entirely about arranging bytes.

## 1.2 RAM vs disk

**What:** A computer stores data in two main places.

| Place | Speed to read one small item | Kept after power off? |
|---|---|---|
| **RAM** (memory) | about 0.0001 ms (100 nanoseconds) | No |
| **SSD** (disk) | about 0.1 ms | Yes |

RAM is about **1,000 times faster** than an SSD. The cost: RAM is emptied when the program stops.

**Why:** Redis keeps **all** data in RAM. That is the whole reason it is fast. Companies put Redis in front of a slower database (for example Postgres, about 1–10 ms per query) and ask Redis first.
**Where it fits:** Every data structure in Parts 5–7 lives in RAM.

## 1.3 Program, process, thread

**What:**
- A **program** is a file of machine instructions (our compiled `server` file).
- A **process** is a program that is running. It has its own private RAM.
- A **thread** is one sequence of instructions running inside a process. A process starts with 1 thread. It can create more. All threads of one process share the same RAM.

**Why:** Part 4 decides how many threads serve clients (answer: 1). Part 8 adds a few extra threads for one special job.

## 1.4 The operating system and system calls

**What:** Your program cannot touch the network card or the disk directly. Only the **operating system** (Linux) can. Your program asks Linux to do it by calling special functions called **system calls**. Examples:

| System call | What it asks Linux to do |
|---|---|
| `socket()` | create a new network endpoint |
| `read(fd, buf, n)` | copy up to `n` received bytes into `buf` |
| `write(fd, buf, n)` | send `n` bytes from `buf` |
| `close(fd)` | close the endpoint |
| `poll(...)` | wait until some endpoints have data |

> [!IMPORTANT]
> These system calls are **C functions**. Linux exposes them as a C interface called **POSIX**. Any C++ network program on Linux calls these C functions at its lowest layer. Remember this for Part 11 (the interview answer).

**Why:** Every network action in our server is one of these calls.
**Where it fits:** Parts 2 and 4.

## 1.5 File descriptor

**What:** When Linux opens something for you (a file or a network connection), it gives you back a small integer called a **file descriptor** (**fd**). You pass that integer to `read()` / `write()` / `close()` to say *which* thing you mean.

```
fd 3 → the server's "waiting for new clients" endpoint
fd 4 → connection to client A
fd 5 → connection to client B
```

**Why:** Our server will have one fd per connected client. Managing these numbers is the main job of the server.

---

# Part 2 — Networking

## 2.1 Client and server

**What:** A **server** is a program that waits for requests. A **client** is a program that sends requests to it. In our project: `server` = our Redis clone, `client` = our test program (later, any program that wants to store data).

## 2.2 IP address and port

**What:**
- An **IP address** identifies a computer on a network. `127.0.0.1` is a special address meaning **"this same computer"**. We use it for all testing.
- A **port** is a number from 0 to 65535. It identifies **which program** on that computer should receive the data. Our server will use port **1234**. (Real Redis uses 6379.)

```
"127.0.0.1 : 1234"  =  "this computer, the program listening on port 1234"
```

## 2.3 TCP

**What:** **TCP** is the set of rules Linux uses to move bytes between two programs. TCP guarantees two things:
1. Bytes arrive **in the same order** they were sent.
2. **No bytes are lost.** If the network drops a piece, TCP sends it again automatically.

Before sending data, the two programs make a **connection** (a long-lived link). After that, each side can send bytes to the other until one side closes it.

## 2.4 Socket

**What:** A **socket** is the fd that represents one end of a network connection. Writing bytes into a socket sends them to the other side. Reading from it returns bytes the other side sent.

## 2.5 How a connection starts — step by step

```
          SERVER                                   CLIENT
  1. fd = socket()        "make an endpoint"
  2. bind(fd, port 1234)  "use port 1234"
  3. listen(fd)           "accept connections"
                                               4. cfd = socket()
                                               5. connect(cfd, 127.0.0.1:1234)
  6. conn = accept(fd)  ◄───── connection made ─────┘
     returns a NEW fd for this one client
  7. read(conn) / write(conn)  ◄──── bytes ────►  write(cfd) / read(cfd)
  8. close(conn)                                  close(cfd)
```

Important: the **listening fd** (step 1) is only used to accept new clients. Each accepted client gets its **own new fd** (step 6). 100 clients = 1 listening fd + 100 connection fds.

**Where it fits:** Step 1 of the build plan is exactly this diagram.

## 2.6 The problem: TCP does not keep message edges

**What:** TCP delivers a **stream of bytes**, not separate messages. If the client calls `write("hello")` and then `write("world")`, the server's `read()` calls might return:

```
read #1 → "hel"
read #2 → "loworld"
```

or `"helloworld"` in one read, or 10 reads of 1 byte. All are correct TCP behaviour. **One `write()` on one side is not one `read()` on the other side.**

**Why it matters:** The server must know where one command ends and the next begins. TCP will not tell it. We solve this ourselves in Part 3.

---

# Part 3 — Our message format (the "protocol")

**What:** A **protocol** is a rule both sides agree on for arranging bytes. Ours: **every message starts with a 4-byte number saying how many bytes follow.**

```
┌────────────────────┬─────────────────────────────┐
│ length (4 bytes)   │ body (exactly `length` bytes)│
└────────────────────┴─────────────────────────────┘
```

The server's rule: *"Read the first 4 bytes → now I know the length L → wait until L more bytes have arrived → now I have one complete message."*

## 3.1 Encoding a command

A command is a list of words. `get a` is the list `["get", "a"]`. The body is: how many words, then for each word its length and its letters.

Worked example — `get a`:

```
part                     value     bytes
-----------------------  --------  ------------------
number of words          2         2 0 0 0
length of word 1         3         3 0 0 0
word 1                   "get"     103 101 116
length of word 2         1         1 0 0 0
word 2                   "a"       97
                                   ───────────────────
body size = 4 + 4 + 3 + 4 + 1 = 16 bytes

full message = [16 0 0 0] + the 16 body bytes = 20 bytes in total
```

## 3.2 Encoding an answer

The answer also starts with the 4-byte length. Then the first body byte is a **type tag** that says what kind of answer follows:

| Tag | Type | What follows | Example |
|---|---|---|---|
| 0 | nil | nothing | `get` on a key that does not exist |
| 1 | error | 4-byte length + message text | `"unknown command"` |
| 2 | string | 4-byte length + text | `"chard"` |
| 3 | integer | 8-byte number | `1` |
| 4 | double | 8-byte decimal number | `310.5` |
| 5 | array | 4-byte count, then that many answers | result of `keys` |

**Why:** Without a length, the server cannot find message edges (Part 2.6). Without a tag, the client cannot tell `"1"` (text) from `1` (number).
**Where it fits:** Build steps 2 and 6. In code: a `Protocol` parser class and a `ResponseWriter` class.

---

# Part 4 — Serving many clients at the same time

## 4.1 The problem: `read()` waits

**What:** By default, `read()` **waits** (the thread stops) until at least one byte arrives. This is called a **blocking** call.

With one thread and two clients:

```
time →
client A connects, sends nothing for 10 seconds
client B connects, sends "get a"

server thread:  read(A) ........... waiting 10 s ........... → only now can it look at B
```

Client B waits 10 seconds for a 0.01 ms job. One slow client stops everyone.

## 4.2 Three possible solutions

| Solution | How it works | Memory per client | Problem |
|---|---|---|---|
| 1 process per client | copy the whole server for each client | several MB | 10,000 clients = tens of GB |
| 1 thread per client | one thread waits on each client | 64 KB – 8 MB | many threads touch the same data → need locks; slow switching |
| **1 thread + event loop** | one thread asks Linux "who has data?" and serves only those | a few hundred bytes | must never wait on any single client |

We use the **third one**. Real Redis, Nginx and Node.js use it too.

## 4.3 Non-blocking mode

**What:** We tell Linux to make an fd **non-blocking**:

```cpp
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
```

After this, `read()` **never waits**. If no data is there, it returns `-1` immediately and sets `errno = EAGAIN` ("nothing now, try later").

## 4.4 `poll()` — asking "which clients are ready?"

**What:** `poll()` is a system call. You give it a list of fds and, for each fd, what you care about:
- `POLLIN` = "tell me when this fd has bytes to read"
- `POLLOUT` = "tell me when I can write to this fd"

`poll()` **waits until at least one fd is ready** (or a timeout passes), then marks which ones are ready.

```
you pass in:                         poll() fills in:
fd 3 (listening)  want POLLIN   →    ready: POLLIN  (a new client is waiting)
fd 4 (client A)   want POLLIN   →    ready: nothing
fd 5 (client B)   want POLLIN   →    ready: POLLIN  (B sent bytes)
```

While waiting, the thread uses **0% CPU**. Linux wakes it only when something happens.

## 4.5 The event loop

**What:** The **event loop** is a `while (true)` loop around `poll()`:

```
while (true) {
    1. make the poll list: listening fd + every connection fd
    2. poll(list, timeout)                   ← the only place we wait
    3. if listening fd is ready  → accept() the new client, make it non-blocking
    4. for each connection that is ready:
         readable → read() bytes into its incoming buffer, then run every complete command
         writable → write() bytes from its outgoing buffer
    5. close connections that ended or failed
    6. handle timers (Part 7)
}
```

## 4.6 Why each connection needs two buffers

Because of Part 2.6, a `read()` can return **half a command**. We cannot throw those bytes away. So each connection keeps:

| Buffer | Holds | Example |
|---|---|---|
| `incoming` | bytes received but not yet a complete message | first 9 of 20 bytes of `get a` |
| `outgoing` | answer bytes not yet sent | Linux accepted only part of the answer |

When `incoming` holds ≥ 4 bytes, read the length L. When it holds ≥ 4 + L bytes, one full command is there: run it and remove those bytes from the front of `incoming`. A client may send several commands at once ("pipelining"), so repeat until no complete message is left.

**Why:** This is how one thread serves thousands of clients without ever waiting on a single one.
**Where it fits:** Build step 3. In code: `Connection` class and `Server::run()`.

> [!NOTE]
> With one thread, only one piece of code ever touches the data at a time. So the data needs **no locks**. This is a big reason the design is fast and simple.

---

# Part 5 — Storing keys: the hashtable

## 5.1 The problem

**What:** We store pairs: key `"name"` → value `"chard"`. The main operation is **"find the value for this key"**.

Simple idea: keep all pairs in a `std::vector` and check each one. With 1,000,000 keys, one `get` checks up to 1,000,000 pairs. Cost grows with the number of keys: **O(n)**.

We want the cost to stay about the same no matter how many keys: **O(1)**.

## 5.2 The hashtable idea

**What:** A **hashtable** is an array of **slots** (also called **buckets**). A **hash function** turns a key into a big number. `number % number_of_slots` gives the slot index. The pair is stored in that slot.

To find a key later: compute the same hash → same slot → look only there.

Worked example with a **toy** hash (sum of letter codes) and 8 slots:

```
"cat" → 99 + 97 + 116 = 312 → 312 % 8 = 0 → slot 0
"dog" → 100 + 111 + 103 = 314 → 314 % 8 = 2 → slot 2
"act" → 97 + 99 + 116 = 312 → 312 % 8 = 0 → slot 0   ← same slot as "cat"!
```

Two keys in the same slot is a **collision**. We handle it by keeping a **short list** in each slot ("chaining"):

```
slot 0 → ("cat", ...) → ("act", ...)
slot 1 → empty
slot 2 → ("dog", ...)
...
```

The toy hash gives "cat" and "act" the same number because it ignores letter order. We will use a real hash function (**FNV-1a**) that mixes order in, so collisions are rare.

## 5.3 Resizing

**What:** If 1,000,000 keys sit in 8 slots, each list has ~125,000 items and we are back to O(n). So when `keys / slots` (the **load factor**) gets too high, we create an array **twice as big** and move every key into it.

## 5.4 The problem with resizing, and progressive rehashing

Moving 10,000,000 keys takes about 1 second. During that second, **every client waits** (one thread, Part 4).

**Progressive rehashing** fixes this:
1. Create the new, bigger table. Keep the old one.
2. On every request, move only **128 keys** from old to new.
3. Lookups check **both** tables until the old one is empty.

No single request ever pays for the full move.

```
old table (shrinking)        new table (growing)
[ ][x][x][x][x]   ──128 per request──►   [x][x][ ][ ][ ][ ][ ][ ]
```

**Why our own, not `std::unordered_map`?**
- `std::unordered_map` moves **all** keys at once when it resizes → the 1-second pause above.
- Your resume says you **designed the hashtable**. You need to explain your own.

**Where it fits:** Build step 5. In code: a class template `HashMap<K, V>`. The keyspace (all keys) and every sorted set (Part 6) use it.

---

# Part 6 — Sorted sets: the AVL tree

## 6.1 What a sorted set is

**What:** A **sorted set** is one key whose value is a collection of **(name, score)** pairs, kept **ordered by score**. Typical use: a game leaderboard.

```
key "board":
   (bob, 250)  (alice, 310)  (eve, 500)      ← always in score order
```

Commands we need and what they require:

| Command | Needs |
|---|---|
| `zadd board 310 alice` | insert or update one pair |
| `zscore board alice` | find a score **by name** |
| `zrem board alice` | delete **by name** |
| `zquery board 300 "" 0 10` | "first 10 pairs with score ≥ 300" — needs **order** |

A hashtable (Part 5) is fast for "by name" but keeps **no order**. So we need a second structure that keeps order.

## 6.2 Binary search tree (BST)

**What:** A **tree** made of **nodes**. Each node holds one value and has up to two children: **left** and **right**. The rule: everything in the left side is **smaller**, everything in the right side is **larger**.

```
            310
           /   \
        250     500
```

To find 500: start at 310 → 500 is larger → go right → found. Each step **discards half** of the remaining tree, so a tree with 1,000,000 nodes needs only about **20 steps** (because 2²⁰ ≈ 1,000,000). This cost is **O(log n)**.

## 6.3 The problem: a BST can become a line

If items are inserted already sorted (10, 20, 30, 40, ...), each new item goes to the right of the last one:

```
10
  \
   20
     \
      30
        \
         40          ← this is a list; finding 40 takes 4 steps, 1,000,000 items take 1,000,000 steps
```

## 6.4 The AVL tree — a BST that keeps itself short

**What:** An **AVL tree** (named after Adelson-Velsky and Landis, 1962) is a BST with one extra rule:

> For **every** node, the **height** of the left side and the height of the right side differ by **at most 1**.

(**Height** = number of nodes on the longest path from that node down to the bottom.)

After every insert or delete, we walk back up and check the rule. If a node breaks it, we fix it with a **rotation**: a re-arrangement of 3 nodes that keeps the left-smaller/right-larger order but makes the tree shorter.

Worked example — insert 10, 20, 30:

```
after inserting 30:             rotate left at 10:
   10   ← left height 0,                20
     \     right height 2                /  \
      20    → rule broken              10    30
        \
         30
```

Result: a 1,000,000-item AVL tree is at most about **29 levels** tall, so every operation stays O(log n).

## 6.5 One more field: subtree size

`zquery board 300 "" 1000 10` means "skip the first 1000 matches, then return 10". Walking past 1000 nodes one by one is slow. So each node also stores **size = number of nodes in its subtree**. Using these sizes, we can jump to "the 1000th node" in O(log n) steps.

## 6.6 The sorted set = 2 structures together

```
SortedSet
 ├── HashMap<name, score>                 → zscore / zrem: find by name in O(1)
 └── AVLTree<(score, name)>               → zquery: walk in score order in O(log n)
```

Every `zadd` and `zrem` updates **both**.

**Why our own, not `std::map`?**
- `std::map` is a **red-black tree**, not an AVL tree. Your resume says AVL.
- `std::map` has **no subtree size**, so "skip 1000 items" costs 1000 steps.

**Where it fits:** Build steps 7 and 8. In code: class template `AVLTree<T>` and class `SortedSet`.

---

# Part 7 — Time: idle timeouts and key expiry

## 7.1 Idle connections

**What:** A client that connects and then sends nothing still uses an fd and memory. We close any connection that has been silent for **5 seconds**.

**How:** keep all connections in a **list ordered by last activity**. When a connection sends anything, move it to the **end** of the list. Then the **front** of the list is always the connection that has been silent the longest. We only ever check the front.

```
front                                          end
[C: silent 6 s] → [A: silent 2 s] → [B: active now]
      ↑ check only this one: 6 s > 5 s → close it, then check the next
```

This works because every connection has the **same** 5-second limit, so "oldest activity" = "expires first".

In C++: `std::list<Connection*>`, and each `Connection` stores its own `std::list` iterator so it can move itself to the end in O(1).

## 7.2 Key expiry (TTL)

**What:** **TTL** = "time to live". `pexpire name 5000` means "delete `name` 5000 ms from now". `pttl name` returns how many ms are left.

**Problem:** each key has a **different** deadline, so a simple ordered list does not work (a new key may expire before older ones). And checking all 1,000,000 keys on every loop would be far too slow.

We need: **"which key has the smallest deadline?"** fast.

## 7.3 The min-heap

**What:** A **min-heap** is a binary tree stored inside a plain array, with one rule: **every parent ≤ its children**. Therefore the **smallest value is always at index 0**.

The array positions map to tree positions by arithmetic: children of index `i` are at `2i+1` and `2i+2`, parent is at `(i-1)/2`.

```
array:  index  0    1    2    3    4
        value  100  300  200  900  400        (deadlines in ms)

tree:            100            ← index 0 = smallest = next key to expire
               /     \
            300       200
           /   \
        900     400
```

| Operation | How | Cost |
|---|---|---|
| look at next deadline | read index 0 | O(1) |
| add a deadline | put at the end, swap upward while smaller than parent | O(log n) |
| remove a deadline | move the last item into its place, swap up or down to fix | O(log n) |

**Why not `std::priority_queue`?** It can only remove the **smallest** item. We also need to remove or change **any** key's deadline (when the user runs `del name` or calls `pexpire` again). So we write our own `ExpiryHeap` class that also remembers **where** each key sits in the array.

## 7.4 Connecting timers to the event loop

`poll()` takes a **timeout**. We set it to "time until the nearest deadline":

```
timeout = min( front of idle list + 5000 ms , heap[0] ) − now
```

So the server sleeps exactly until the next thing must happen, then wakes, expires what is due, and continues.

**Where it fits:** Build steps 9 and 10. In code: the idle `std::list` inside `Server`, and class `ExpiryHeap`.

---

# Part 8 — Slow work: the thread pool

**What:** The main thread runs every command for every client. If one command takes 100 ms, every client waits 100 ms.

One operation is slow by nature: **deleting a sorted set with 1,000,000 items** frees 1,000,000 nodes. That takes tens of milliseconds.

A **thread pool** is a fixed number of extra threads (we use **4**) that wait for jobs in a shared **queue**:

```
main thread                         worker threads
-----------                         --------------
del bigset                          [W1] [W2] [W3] [W4]
  → remove key from keyspace           ↑ one of them takes the job
  → push "free this SortedSet" ──► [ job queue ]
  → answer the client at once       → W2 frees the 1,000,000 nodes
```

Two C++ tools make the shared queue safe:

| Tool | What it does |
|---|---|
| `std::mutex` | only one thread at a time may touch the queue |
| `std::condition_variable` | lets a worker sleep until the main thread adds a job |

This is safe because once the sorted set is removed from the keyspace, **no other code can reach it**. The worker is the only one touching it.

**Where it fits:** Build step 11. In code: class `ThreadPool`.

---

# Part 9 — The full design in C++ classes

## 9.1 High-level picture

```
                      ┌───────────────────────────── Server ──────────────────────────────┐
  client ──TCP──►     │                                                                   │
  client ──TCP──►     │  listen Socket ─accept()─► Connection (one per client)            │
  client ──TCP──►     │                              │ incoming bytes                     │
                      │  run(): poll() loop          ▼                                    │
                      │                        Protocol::try_parse()                      │
                      │                              │ ["zadd","board","310","alice"]     │
                      │                              ▼                                    │
                      │                        KeySpace::execute()  ──► ResponseWriter    │
                      │                         │        │                 │ outgoing     │
                      │       HashMap<string, Entry>     │                 ▼ bytes        │
                      │         Entry = string           │            back to client      │
                      │              or SortedSet        │                                │
                      │           (HashMap + AVLTree)    │                                │
                      │                                  ▼                                │
                      │  idle std::list   ExpiryHeap   ThreadPool (4 std::threads)        │
                      └───────────────────────────────────────────────────────────────────┘
```

## 9.2 Every class, one line each

| Class | Job | Main C++ parts used |
|---|---|---|
| `Socket` | owns one fd; closes it automatically when destroyed | RAII, move-only class |
| `Connection` | one client: socket + 2 buffers + state | `std::vector<uint8_t>`, `std::list::iterator` |
| `Protocol` | turns incoming bytes into a command (or "not complete yet") | `std::optional`, `std::vector<std::string>` |
| `ResponseWriter` | appends tagged answers to the outgoing buffer | `std::vector<uint8_t>` |
| `HashMap<K,V>` | our hashtable with progressive rehashing | class template, `std::unique_ptr` |
| `AVLTree<T>` | our balanced tree with subtree sizes | class template, `std::unique_ptr` |
| `SortedSet` | `HashMap` + `AVLTree` kept in sync | composition |
| `KeySpace` | all keys; runs commands | `HashMap<std::string, Entry>`, `std::variant` |
| `ExpiryHeap` | min-heap of (deadline, key) | `std::vector`, own index map |
| `ThreadPool` | 4 workers + job queue | `std::thread`, `std::mutex`, `std::condition_variable` |
| `Server` | owns everything above; runs the loop | `std::unordered_map<int, std::unique_ptr<Connection>>` |

> [!NOTE]
> We use standard containers for **plumbing** (buffers, connection list, job queue). We write our **own** `HashMap`, `AVLTree` and `ExpiryHeap`, because those are the three things your resume says you designed.

**RAII** (used in `Socket`): "Resource Acquisition Is Initialization". The object gets the resource (the fd) in its constructor and releases it (`close(fd)`) in its destructor. So an fd can never be forgotten open, even if an error happens.

## 9.3 Sketches of the main classes

```cpp
class Socket {
public:
    explicit Socket(int fd);
    ~Socket();                               // calls close(fd_)
    Socket(Socket&&) noexcept;               // can be moved
    Socket(const Socket&) = delete;          // cannot be copied (two owners of one fd = bug)
    int fd() const;
private:
    int fd_ = -1;
};

class Connection {
public:
    Socket socket;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
    bool want_close = false;
    uint64_t last_active_ms = 0;
    std::list<Connection*>::iterator idle_pos;   // where it sits in the idle list
};

template <class K, class V>
class HashMap {
public:
    V*   find(const K& key);
    void insert(K key, V value);
    bool erase(const K& key);
    size_t size() const;
private:
    struct Node { K key; V value; uint64_t hash; std::unique_ptr<Node> next; };
    struct Table { std::vector<std::unique_ptr<Node>> slots; size_t count = 0; };
    Table newer_, older_;          // two tables during progressive rehashing
    size_t migrate_pos_ = 0;
    void help_rehash();            // moves up to 128 keys per call
};

template <class T>
class AVLTree {
public:
    void insert(const T& v);
    bool erase(const T& v);
    const T* lower_bound(const T& v) const;          // first item >= v
    const T* at_offset(const T* from, long k) const;  // jump k items, O(log n)
private:
    struct Node { T value; int height = 1; size_t size = 1;
                  std::unique_ptr<Node> left, right; };
    std::unique_ptr<Node> root_;
};

class SortedSet {
    HashMap<std::string, double> by_name_;
    AVLTree<std::pair<double, std::string>> by_score_;
};

struct Entry {
    std::variant<std::string, SortedSet> value;   // a key holds text OR a sorted set
};
```

## 9.4 Life of one request — `set name chard`

```
 1. client sends 32 bytes (4-byte length + 28-byte body)
 2. poll() reports: client fd readable
 3. Connection reads them → incoming now has 32 bytes
 4. Protocol::try_parse(incoming):
        first 4 bytes say body = 28 → incoming has 4 + 28 → complete
        → returns ["set", "name", "chard"]
 5. KeySpace::execute():
        HashMap.find("name") → not found → insert ("name", Entry{"chard"})
 6. ResponseWriter appends [tag 0 = nil] to outgoing
 7. the 32 bytes are removed from the front of incoming
 8. poll() reports: client fd writable → write(outgoing)
 9. client prints (nil)
```

---

# Part 10 — Build plan

Each step ends with something you can run. Each step = one commit.

| # | Step | Book ch. | You know it works when | Hours |
|---|---|---|---|---|
| 1 | `Socket` class; server + client say hello | 2–3 | client prints the reply | 2 |
| 2 | `Protocol`: length-prefixed messages | 4 | 3 messages → 3 replies | 1.5 |
| 3 | `Connection` + `Server::run()` with `poll()` | 5–6 | 2 clients, neither waits | 3 |
| 4 | get / set / del on `std::unordered_map` (temporary) | 7 | `set a 1` then `get a` | 1.5 |
| 5 | own `HashMap` with progressive rehashing; replace step 4's map | 8 | 100k keys insert + find | 3 |
| 6 | `ResponseWriter` tags + `keys` | 9 | `keys` prints an array | 1.5 |
| 7 | `AVLTree` with sizes | 10 | random test keeps balance | 3 |
| 8 | `SortedSet` + z-commands | 11 | leaderboard script | 3 |
| 9 | idle timeout with `std::list` | 12 | silent client dropped at 5 s | 1.5 |
| 10 | `ExpiryHeap`: pexpire / pttl | 13 | key gone after N ms | 2 |
| 11 | `ThreadPool` for freeing big sets | 14 | 1M-item delete, no pause | 2 |
| 12 | README, tests, benchmark | — | `make test` passes | 2 |
| | **Total** | | | **~26** |

Compile with `g++ -std=c++20 -Wall -Wextra -O2` in **WSL Ubuntu 24.04** (already installed). `poll()`, `fcntl()` and sockets are Linux functions and do not exist in plain Windows.

---

# Part 11 — Interview: "Your resume says C, but the code is C++"

## 11.1 What is actually true in this project

| Layer | Language |
|---|---|
| Talking to Linux: `socket`, `bind`, `accept`, `read`, `write`, `poll`, `fcntl` | **C** (the POSIX C interface, Part 1.4) |
| Everything we build on top: classes, containers, threads | **C++** |
| The book we follow | titled *"Build Your Own Redis with **C/C++**"* |

## 11.2 What you can say

> "It's a C/C++ systems project. The networking layer is the raw POSIX C API: sockets, `poll()`, `fcntl()` for non-blocking mode. I don't use any networking library. I wrote the rest in C++, so it's C-style systems code with C++ on top. I wrote the hashtable, the AVL tree and the heap myself as class templates, and I used RAII for the sockets and `std::thread` for the pool. I should have written C/C++ on the resume."

Each part of that is true, and it moves the talk to the part that matters: what you built and why.

> [!WARNING]
> Do **not** claim the code is pure C if it is not. An interviewer can ask you to open the repo. A small, honest correction ("should have written C/C++") costs nothing. A false claim that gets caught costs the interview.

## 11.3 Be ready for the follow-up: "How would this look in pure C?"

Learn this table. It shows you understand both.

| Topic | Our C++ version | Pure C version |
|---|---|---|
| Closing fds | `Socket` destructor (RAII) | call `close()` by hand on every path |
| Buffers | `std::vector<uint8_t>` | `malloc` / `realloc` / `free` + size fields |
| Strings | `std::string` | `char*` + length |
| Hashtable generic over types | class template `HashMap<K,V>` | **intrusive** node embedded in the struct + `container_of` macro |
| Text-or-set value | `std::variant` | `struct` with a `type` field + `union` |
| Threads | `std::thread`, `std::mutex` | `pthread_create`, `pthread_mutex_t` |
| Memory freeing | automatic via `std::unique_ptr` | manual `free()`; leaks if forgotten |

**Intrusive structure (the C technique, in one line):** instead of the hashtable allocating a node that holds your data, your data struct contains the hashtable's link field, and `container_of(ptr, Type, field)` subtracts the field's offset to get back to your struct. The book uses this. Know what it is even though we use templates.
