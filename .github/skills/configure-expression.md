# Configuration Expression Expansion (libatapp)

Protobuf fields annotated with `enable_expression: true` in the `atapp_configure_meta` extension
support **environment-variable expression expansion** when configuration is loaded from YAML, INI (`.conf`),
or environment-variable files.

## Expression Syntax

| Syntax | Description |
| --- | --- |
| `$VAR` | Bare variable — POSIX names only (`[A-Za-z_][A-Za-z0-9_]*`) |
| `${VAR}` | Braced variable — any characters allowed, including `.`, `-`, `/` (k8s labels) |
| `${VAR:-default}` | If `VAR` is **unset or empty**, expand to `default` |
| `${VAR:+word}` | If `VAR` is **set and non-empty**, expand to `word`; otherwise empty string |
| `\$` | Literal dollar sign (escape) |
| Nested | `${OUTER_${INNER}}`, `${VAR:-${OTHER:-fallback}}` — arbitrary nesting |

### Bare `$VAR` vs Braced `${VAR}`

- **Bare** `$VAR` stops at the first character that is not `[A-Za-z0-9_]`.
  Characters such as `.`, `-`, `/` are **not** consumed, so `$app.name` resolves only `$app`.
- **Braced** `${VAR}` accepts any characters up to the matching `}`, which makes it suitable for
  Kubernetes-style labels like `${app.kubernetes.io/name}`.

### Default / Alternate Values

Follows **bash** semantics:

- `${VAR:-default}` — triggers when `VAR` is **empty or unset** (empty string counts as unset).
- `${VAR:+word}` — triggers only when `VAR` is **non-empty**.
- `default` and `word` can themselves contain nested expressions: `${A:-${B:-fallback}}`.

## Protobuf Extension

The extension is defined in `atapp_conf.proto`:

```protobuf
message atapp_configure_meta {
  // ...
  bool enable_expression = 6;  // Enable expression expansion for this field
}
```

Usage example:

```protobuf
message atapp_metadata {
  map<string, string> label = 21
      [(atframework.atapp.protocol.CONFIGURE) = { enable_expression: true }];
}
```

For `map` fields, **both key and value** are expanded when the parent field has `enable_expression: true`.

## Loader Integration

Expression expansion is applied in all three config-loading paths:

1. **YAML** — string values are expanded after reading from YAML nodes
2. **INI (`.conf`)** — values are expanded after parsing INI key-value pairs
3. **Environment variables** — values from `.env.txt` files are expanded

The loader checks `enable_expression` on the field's `FieldOptions` extension (or the parent map
field for map entry sub-fields). Only fields that opt in are expanded.

## Public C++ API

```cpp
#include <atframe/atapp_conf.h>

// Expand environment expressions in an arbitrary string.
// Useful outside of config loading (e.g. in custom modules).
std::string result = atframework::atapp::expand_environment_expression("Hello ${USER:-world}");
```

Environment variables are read via `atfw::util::file_system::getenv()` (not `std::getenv()`).

## YAML Config Example

```yaml
# app.yaml
atapp:
  metadata:
    label:
      app.kubernetes.io/name: "${APP_NAME:-my-service}"
      service_subset: "${SUBSET:-${ZONE:-default}}"
  bus:
    listen:
      - "ipv4://0.0.0.0:${LISTEN_PORT:-12345}"
```

## INI Config Example

```ini
[atapp.metadata.label]
app.kubernetes.io/name=${APP_NAME:-my-service}
service_subset=${SUBSET:-${ZONE:-default}}

[atapp.bus]
listen=ipv4://0.0.0.0:${LISTEN_PORT:-12345}
```
