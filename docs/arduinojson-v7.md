# ArduinoJson v7 — API reference for this project

**This project uses ArduinoJson v7. Do not write v6 code.**

Compiled from the official v7 documentation. If anything here conflicts with the installed headers in `.pio/libdeps/`, the headers win — read them.

Most v6 code still *compiles* in v7 with deprecation warnings, so "it built" is not evidence you used the right API. The two entries below marked **⚠ SILENT** change behaviour without any compiler complaint at all.

---

## 1. Do not write these — v6 idioms

| ❌ v6 | ✅ v7 |
|---|---|
| `StaticJsonDocument<256> doc;` | `JsonDocument doc;` |
| `DynamicJsonDocument doc(256);` | `JsonDocument doc;` |
| `doc.capacity()` | **removed** — use `doc.overflowed()` |
| `doc.memoryUsage()` | **removed** — no longer meaningful |
| `doc.garbageCollect()` | **removed** — memory is reused automatically |
| `variant.shallowCopy(other)` | **removed** — `doc1["key"] = doc2` (deep copy) |
| `arr.createNestedArray()` | `arr.add<JsonArray>()` |
| `arr.createNestedObject()` | `arr.add<JsonObject>()` |
| `obj.createNestedArray("k")` | `obj["k"].to<JsonArray>()` |
| `obj.createNestedObject("k")` | `obj["k"].to<JsonObject>()` |
| `JSON_ARRAY_SIZE(n)` | **removed** |
| `JSON_OBJECT_SIZE(n)` | **removed** |
| `BasicJsonDocument<MyAlloc> doc(4096);` | `MyAlloc a; JsonDocument doc(&a);` |

There is no document capacity in v7. `JsonDocument` always allocates on the heap and grows elastically. Any code that sizes a document up front is v6.

`containsKey()` still exists on `JsonDocument`. Prefer `doc["key"].is<T>()` for new code, since it checks presence and type together.

---

## 2. Two silent behaviour changes ⚠

These compile clean and do the wrong thing. Watch for them.

**⚠ SILENT — `serializeJson()` into a `String` now replaces instead of appends.**

```cpp
// v6 required clearing first; in v7 this line is redundant but harmless
outputString = "";
serializeJson(doc, outputString);   // v7: replaces content
```

Same for `serializeJsonPretty()`, `serializeMsgPack()`, and `std::string`. v6 code that relied on appending is now broken with no warning.

**⚠ SILENT — a custom allocator must now `override` three methods, including `reallocate()`.**

`reallocate()` did not exist in v6 and is **required** in v7. Omitting it means the class stays abstract (that one does fail to compile) — but copying a v6 allocator body without `override` and without `reallocate` is the common mistake.

---

## 3. The pattern this project actually uses

Filtered streaming parse. This is the single most important snippet in the codebase — see `AGENTS.md` and PLAN.md §2.

```cpp
// 1. Build the filter: `true` marks each field to KEEP.
//    For arrays, define ONE element — it filters every element.
JsonDocument filter;
JsonObject ev = filter["events"][0].to<JsonObject>();
ev["id"]   = true;
ev["date"] = true;

JsonObject comp = ev["competitions"][0].to<JsonObject>();
JsonObject st   = comp["status"].to<JsonObject>();
st["period"]            = true;
st["displayClock"]      = true;
st["type"]["state"]     = true;
st["type"]["shortDetail"] = true;

JsonObject c = comp["competitors"][0].to<JsonObject>();
c["homeAway"] = true;
c["score"]    = true;
JsonObject tm = c["team"].to<JsonObject>();
tm["id"] = tm["displayName"] = tm["abbreviation"] = tm["color"] = true;

comp["situation"] = true;   // small — keep the whole subtree

// 2. Parse straight off the stream. Never buffer the response.
JsonDocument doc(&psramAllocator);
DeserializationError err = deserializeJson(
    doc, bufferedStream, DeserializationOption::Filter(filter));
if (err) { /* degrade gracefully — see AGENTS.md */ }
```

`deserializeJson()` resets the document first, so you never need `doc.clear()`. It also calls `shrinkToFit()` for you.

Everything not named in the filter is skipped without being allocated. On our MLB scoreboard that is 1.46 MB in, ~4.5 KB retained.

---

## 4. Reading from a stream — buffer it

**By default `deserializeJson()` consumes a `Stream` one byte at a time.** On a 1.46 MB response that is catastrophic. Wrap the stream in `ReadBufferingStream` from the [StreamUtils](https://github.com/bblanchon/ArduinoStreamUtils) library — the official docs cite roughly **20× faster** for file reads with 64-byte chunks.

```cpp
#include <StreamUtils.h>

ReadBufferingStream bufferedStream(httpStream, 512);
deserializeJson(doc, bufferedStream, DeserializationOption::Filter(filter));
```

Use a larger chunk than the doc's 64-byte example — we are reading from TLS over WiFi, not SPIFFS, and internal RAM allows 512–1024 B comfortably.

For debugging what was actually consumed, `ReadLoggingStream` from the same library mirrors the stream to `Serial`.

Accepted input types: `const char*`, `String`, `std::string`, `std::string_view`, `Stream&`, `std::istream&`, `JsonVariantConst`, `__FlashStringHelper*`, or a custom reader implementing `int read()` and `size_t readBytes(char*, size_t)`.

---

## 5. PSRAM allocator (ESP32)

Straight from the official docs. Note `: ArduinoJson::Allocator`, the three `override`s, and `reallocate()`.

```cpp
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* pointer) override {
    heap_caps_free(pointer);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};

SpiRamAllocator allocator;
JsonDocument doc(&allocator);
```

We have 8 MB of octal PSRAM, so the parse document goes here and internal SRAM stays free for the DMA framebuffer and TLS session.

---

## 6. `JsonDocument` essentials

`JsonDocument` has **value** semantics (copying clones). `JsonVariant`, `JsonObject`, `JsonArray` have **reference** semantics (copying aliases). Getting this backwards produces expensive accidental deep copies.

A fresh document is neither object nor array — `isNull()` is true. It takes its type from first use, or explicitly via `to<T>()`.

| Member | Purpose |
|---|---|
| `to<T>()` | **clears** the document and converts it to `T` |
| `as<T>()` | casts the root to `T` without clearing |
| `is<T>()` | tests the root's type |
| `isNull()` | null or empty |
| `overflowed()` | 🆕 v7 — allocation failed partway |
| `shrinkToFit()` | release over-allocation (auto-called by `deserializeJson`) |
| `size()`, `nesting()`, `remove()`, `add()`, `set()`, `clear()` | as expected |

`to<T>()` clears; `as<T>()` does not. Confusing them silently discards a parsed document.

---

## 7. Error handling

`deserializeJson()` returns a `DeserializationError`, which is truthy on failure. Always check it — per our hard rules, a malformed or truncated response must degrade to last-good data, never crash or blank the panel.

Nesting is capped by `ARDUINOJSON_DEFAULT_NESTING_LIMIT` as a stack-overflow guard. ESPN payloads nest deeply; if you hit the limit, raise it explicitly:

```cpp
deserializeJson(doc, input, DeserializationOption::NestingLimit(15));
```

---

## 8. Chunked parsing — fallback only

If filtering alone is not enough, v7 can parse an array element at a time: it stops reading at the closing brace of each object.

```cpp
input.find("\"events\":[");
do {
    deserializeJson(doc, input, DeserializationOption::Filter(filter));
    // handle one event, then reuse doc
} while (input.findUntil(",", "]"));
```

**Arrays only** — a large object cannot be chunked. We do not use this by default; filtering plus the narrowed date window (T-5.6) should suffice. Reach for it only if T-0.5 measurements demand it.

---

## 9. Note on code size

v7 dropped v6's aggressive code-size focus and is **significantly larger**. The docs recommend staying on v6 for 8-bit targets. Irrelevant for us — we have 16 MB of flash — but it explains why v6 examples still dominate search results, and why a model's priors lean v6.

---

## Sources

- [Upgrading from v6 to v7](https://arduinojson.org/v7/how-to/upgrade-from-v6/)
- [`deserializeJson()`](https://arduinojson.org/v7/api/json/deserializejson/)
- [`JsonDocument`](https://arduinojson.org/v7/api/jsondocument/)
- [Deserialize a very large document](https://arduinojson.org/v7/how-to/deserialize-a-very-large-document/)
- [StreamUtils](https://github.com/bblanchon/ArduinoStreamUtils)
