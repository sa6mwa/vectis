# liblockdc Change Request: Pouch Query Option Presence

## Problem

Consumers that need to decide whether to apply a Pouch default currently have
to inspect `pouch://` endpoint text themselves. That is unsafe: liblockdc owns
the endpoint grammar and percent-decodes query names, while an external raw
substring or raw-name comparison can disagree with the options liblockdc will
actually apply.

For example, liblockdc recognizes:

```text
pouch:///srv/state?pouch%5Fcrypto%5Fkey=lc-pouch-key-v1%3A...
```

An external raw check for `pouch_crypto_key` misses that option. A consumer may
then inject a default key file that overrides the endpoint key, preventing an
existing encrypted root from reopening. The inverse error—treating matching
text in a path or another option's value as configuration—can silently bypass
an encryption default.

## Requested API

Expose a small public helper that uses the same endpoint-query parsing and
decoding rules as `lc_client_open`:

```c
int lc_pouch_endpoint_has_option(const char *endpoint, const char *name,
                                 int *present, lc_error *error);
```

- Return `LC_OK` and set `*present` to zero or nonzero.
- Require a non-NULL `endpoint`, non-empty `name`, and non-NULL `present`.
- Reject a non-Pouch endpoint with the normal liblockdc invalid-argument
  diagnostic rather than guessing from arbitrary URI text.
- Do not return, log, or copy the option value. The helper answers presence
  only, which avoids creating a second secret-bearing API.

The name deliberately remains generic. Consumers need this for
`pouch_crypto_key`, `pouch_crypto_key_file`, and future Pouch options without
adding a parser or one helper per option.

## Required Semantics

`lc_pouch_endpoint_has_option` must share the exact parser used when opening a
Pouch endpoint:

1. Inspect only the query component, after `?` and before `#`.
2. Split parameters and decode their names exactly as endpoint opening does,
   including percent escapes and the parser's handling of `+`.
3. Compare the decoded parameter name exactly to `name`.
4. Do not match a path segment, fragment, parameter value, or a longer name.
5. Treat duplicate names as present.
6. Handle a name without `=` exactly as endpoint opening does; it must not
   have different presence semantics here.
7. Surface malformed query escaping and other parse failures with the same
   liblockdc error class and message style used by `lc_client_open`.

The helper must not silently normalize malformed input or implement a
parallel URI/query decoder.

## Acceptance Coverage

Add native liblockdc tests that prove the result agrees with actual endpoint
opening for:

- raw `pouch_crypto_key` and `pouch_crypto_key_file` names;
- percent-encoded and mixed-case percent-encoded names;
- names following unrelated parameters;
- a matching string in the root path;
- a matching string only in another option's value;
- a longer name such as `pouch_crypto_key_file_backup`;
- a matching string in a fragment;
- duplicate names, bare names, empty values, and malformed percent escapes.

For crypto options, include an observable encrypted-root reopen test: a
percent-encoded endpoint key must be recognized as present and must reopen the
same root, while a lookalike string must not suppress the configured default.

## Vectis Follow-up

After this API ships, Vectis should replace its local
`vectis_pouch_endpoint_has_crypto_option` implementation with this helper in
both ordinary lockdc-client setup and ACME-state setup. Vectis must continue
to generate its private default key only when neither a configured key/key file
nor an actual liblockdc crypto query option is present.
