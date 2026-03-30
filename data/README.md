# Test Data

## File Format: NASDAQ TotalView-ITCH 5.0

ITCH 5.0 is a binary protocol used by NASDAQ to disseminate full order-book
data. It is the authoritative source for all add/cancel/execute/replace events
for every listed security.

### Binary encoding

Every message in the stream is preceded by a **2-byte big-endian length
prefix** that gives the number of bytes in the message body (the length field
itself is not counted). The first byte of the message body is the message type.

```
 0        1        2  ...  2+len-1
+--------+--------+------------ ... ---+
| len_hi | len_lo | message body (len bytes) |
+--------+--------+------------ ... ---+
```

All multi-byte integer fields inside messages are **big-endian** (network byte
order). Prices are encoded as `uint32_t` with an implicit 4-decimal-place
divisor: a price of `$10.25` is stored as `102500`.

### Message types implemented

| Type | Name                    | Body size |
|------|-------------------------|-----------|
| `S`  | System Event            | 12 bytes  |
| `A`  | Add Order (no MPID)     | 36 bytes  |
| `F`  | Add Order (with MPID)   | 40 bytes  |
| `E`  | Order Executed          | 31 bytes  |
| `X`  | Order Cancel (partial)  | 23 bytes  |
| `D`  | Order Delete (full)     | 19 bytes  |
| `U`  | Order Replace           | 35 bytes  |
| `P`  | Non-Cross Trade         | 44 bytes  |

### Obtaining real NASDAQ ITCH 5.0 data

NASDAQ publishes historical ITCH 5.0 sample files (gzip-compressed) at:

```
ftp://emi.nasdaq.com/ITCH/
```

Files are named `YYYYMMDD.NASDAQ_ITCH50.gz`. A full trading-day file is
typically 5–12 GB uncompressed.

To download and decompress:
```bash
wget ftp://emi.nasdaq.com/ITCH/01302019.NASDAQ_ITCH50.gz
gunzip 01302019.NASDAQ_ITCH50.gz
```

The golden model reads the raw uncompressed binary directly. Place the file
here as `data/<filename>` and pass it to `--file`.

### Synthetic test data

The `tools/generate_test_data` binary creates a synthetic `.itch` file with
1 000 000+ messages covering all supported message types. This is used for
`make test` and `make bench` when real Nasdaq data is not available.

```bash
# From repo root:
make test-data            # builds generator and writes data/sample.itch
```

The generator produces a statistically realistic but **not market-accurate**
order stream — sufficient for parser correctness testing and throughput
benchmarking, but not for strategy research.
