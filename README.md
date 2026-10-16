# sdrouter

A userspace software-defined router in C++. It receives Ethernet frames on raw
sockets, does a longest-prefix-match route lookup, rewrites the headers, and
forwards them out another interface - while the control plane rewrites the
routing table underneath it, without stopping traffic.

The architecture is the split real routers make:

- **Control plane** decides routes. Slow path, single-threaded event loop,
  line-oriented TCP protocol.
- **Data plane** moves packets. Fast path, one thread per port, no locks.

```
                       control plane
        +---------------------------------------+
        |  TCP :5555   ADD_ROUTE / DEL_ROUTE     |
        |      |                                 |
        |      v                                 |
        |  Published<T>::update                  |
        |      |  copy -> mutate -> publish      |
        +------|--------------------------------+
               |  atomic pointer swap + QSBR grace period
               v
        +---------------------------------------+
        |  immutable snapshot (RouteTable trie)  |
        +---------------------------------------+
               ^          ^          ^
               |   Published<T>::read (lock-free)
        +------|----------|----------|----------+
        |   rt1 jthread rt2 jthread rt3 jthread |   data plane
        |   AF_PACKET   AF_PACKET   AF_PACKET   |
        +---------------------------------------+
```

## Quick start

```sh
make                     # configure + build via CMake
make test                # unit + concurrency tests, no root needed
make bench               # QSBR vs atomic<shared_ptr> measurement
sudo ./scripts/demo.sh   # end-to-end demo with veth + network namespaces
```

Running it by hand:

```sh
sudo ./scripts/topology.sh up
sudo ./scripts/topology.sh conf /tmp/demo.conf
sudo ./build/sdrouter -i rt1 -i rt2 -i rt3 -f /tmp/demo.conf

# from another shell
./tools/rctl.py show routes
./tools/rctl.py add-route 30.0.0.0/24 rt3 --via 10.0.3.10
./tools/rctl.py show stats
```

Raw sockets need `CAP_NET_RAW`: run as root, or
`sudo setcap cap_net_raw,cap_net_admin=eip ./build/sdrouter`.

## The concurrency design, and why not `atomic<shared_ptr>`

The control plane must install a route while three data-plane threads are
mid-lookup. A reader-writer lock per packet would work and would also put an
atomic read-modify-write on the hottest path in the program.

Instead, `Published<T>::update` copies the table, mutates the copy, swaps the
pointer with a release store, waits out a QSBR grace period, then frees the old
one. Readers do two atomic stores and a load — no lock, no CAS, no allocation.

C++ offers `std::atomic<std::shared_ptr<const T>>`, which gets reclamation for
free and is the idiomatic answer. It was the obvious candidate for this port, so
`tests/bench_snapshot.cpp` measures both rather than arguing about it:

```
atomic<shared_ptr> lock-free: no

atomic<shared_ptr>         13.77 M lookups/sec
Published<T> (QSBR)        71.43 M lookups/sec
```

Two things to note. `atomic<shared_ptr>` is **not lock-free** on libstdc++, it
falls back to a spinlock table, and every read touches a refcount, which is
exactly the per-packet atomic RMW the design is trying to avoid. And this
measurement is from a single-core container, which exaggerates contention; the
ratio would compress on real hardware. The direction is what matters, and the
benchmark is in the repo so the number can be re-measured anywhere.

`shared_ptr` would be the right call if reclamation timing were unpredictable or
readers were not a bounded, registered set. Here they are exactly one per port.

## Forwarding path

Per packet, `data_plane.cpp`:

- ignores frames marked `PACKET_OUTGOING` - AF_PACKET echoes our own
  transmissions back, which would otherwise loop every forwarded frame
- ignores `PACKET_OTHERHOST`; promiscuous mode delivers frames addressed to
  other stations, and a router forwards only what was sent to its own MAC
- validates EtherType, IP version, IHL, total length against the real frame
  length, and the header checksum
- drops at TTL ≤ 1 rather than forwarding a packet that dies on the next hop
- decrements TTL and patches the checksum **incrementally** (RFC 1624) rather
  than recomputing the header - constant work, and what real forwarding
  hardware does
- refuses to send a packet back out the port it arrived on
- trims Ethernet padding, forwarding exactly what IP's total length claims

Every drop reason has its own counter, visible in `SHOW STATS`. Counters are
written only by their own port's thread, so they use relaxed ordering.

## Control protocol

Line in, block out. Every reply ends with a line starting `OK` or `ERR`, which
lets a client read a response of unknown length without guessing a timeout.

```
ADD_ROUTE <a.b.c.d/len> <iface> [VIA <a.b.c.d>]
DEL_ROUTE <a.b.c.d/len>
ADD_NEIGH <a.b.c.d> <aa:bb:cc:dd:ee:ff>
DEL_NEIGH <a.b.c.d>
SHOW ROUTES | SHOW NEIGH | SHOW IFACES | SHOW STATS
PING | HELP | QUIT
```

Commands dispatch through a table rather than an if-else chain, so adding one is
a single entry. The same syntax works as a config file via `-f`, so startup
state and runtime reconfiguration share one parser.

## Testing

```
make test    # unit + concurrency tests via CTest
make asan    # rebuild under AddressSanitizer + UBSan, rerun
make tsan    # rebuild under ThreadSanitizer, rerun
```

Unit tests cover the address value types (including round-tripping host vs
network order and rejecting malformed input), LPM semantics, `RouteTable` value
semantics - that a copy is a deep clone, which is the property `Published<T>`
depends on - the checksum helpers, including a proof that the incremental TTL
update is byte-identical to a full recompute for every TTL from 2 to 255, and
frame validation against runts, wrong EtherTypes, bad IHL, lying length fields,
corrupted checksums, and padded frames.

`test_concurrency.cpp` runs four readers against one writer and asserts that a
route the writer never touches is visible on every single lookup. Clean under
both TSan and ASan; the latter is the real check, since a reclamation bug shows
up as a use-after-free.

`scripts/ci_integration.sh` is the end-to-end check: traffic crosses the router,
the reply TTL is 63 rather than 64 (proving sdrouter forwarded it and not the
kernel), TTL=1 is dropped, and a live route withdrawal stops traffic while a
reinstall resumes it. CI runs everything under both g++ and clang++.

## Layout

```
include/sdr/
  address.hpp          Ipv4Address, MacAddress, Prefix value types
  packet.hpp           wire formats, validation, checksums
  qsbr.hpp             quiescent-state reclamation + RAII guard
  published.hpp        Published<T>: lock-free snapshot publication
  route_table.hpp      LPM binary trie with value semantics
  neighbor_table.hpp   static IP -> MAC table
  interface.hpp        AF_PACKET port + counters
  file_descriptor.hpp  move-only fd ownership
  data_plane.hpp       per-port forwarding threads
  control_plane.hpp    TCP control protocol
  router.hpp           owns the pieces
src/                   implementations
tests/                 unit, concurrency, and the snapshot benchmark
tools/rctl.py          control plane CLI
scripts/               topology setup, demo, CI integration test
```
