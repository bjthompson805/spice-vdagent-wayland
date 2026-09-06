# wlr-protocol bindings

`wlr-data-control-unstable-v1-client-protocol.h` and
`wlr-data-control-unstable-v1-protocol.c` are generated from the vendored
`wlr-data-control-unstable-v1.xml` (from
[wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols)) via:

```
wayland-scanner client-header wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-client-protocol.h
wayland-scanner private-code  wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-protocol.c
```

Committed directly rather than generated at build time to avoid adding a
`wlr-protocols` checkout as a build-time dependency for what is currently
a single protocol file. Regenerate after updating the vendored XML.
