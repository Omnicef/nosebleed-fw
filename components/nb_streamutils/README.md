# nb_streamutils

Header-only vendored copy of [StreamUtils](https://github.com/bblanchon/ArduinoStreamUtils)
v1.9.2 (MIT, (c) Benoit Blanchon) — `src/` copied verbatim from the
PlatformIO libdeps tree, which resolved `v1.9.2` (tag object
`502aead44a07e6760e5d26f9d34b3cb909dc5924` → commit
`f83978f46eb7b5799664548bc40acc19b25d083e`); `StreamUtils.hpp` md5-matches
the `v1.9.2` tag on GitHub.

Vendored, not a managed component, because the upstream `CMakeLists.txt` is
its host-test harness rather than an IDF component file. Only
`ReadBufferingStream` is used (512 B chunks off the TLS stream, T-0.5/T-5.3).
Upgrade = re-copy `src/` and bump this note.
