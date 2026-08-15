#!/usr/bin/env python3
import pathlib
import re
import sys


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    header_path = repo_root / ".cache/deps/host-debug/include/cpkt/opcua.h"
    source_path = repo_root / "src/vectis_opcua_lua.c"

    if not header_path.exists():
        print(
            f"missing {header_path}; run `make deps-debug` before the OPC UA Lua surface audit",
            file=sys.stderr,
        )
        return 1

    header = header_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")
    symbols = sorted(
        set(
            re.findall(
                r"\b(cpkt_opcua_(?:server|client)_[A-Za-z0-9_]+)\s*\(",
                header,
            )
        )
    )

    missing = [symbol for symbol in symbols if symbol not in source]
    unexpected_missing = [
        symbol for symbol in missing if "native" not in symbol
    ]
    client_pubsub = [
        symbol
        for symbol in symbols
        if symbol.startswith("cpkt_opcua_client_") and "pubsub" in symbol
    ]

    if unexpected_missing:
        print("OPC UA Lua facade is missing non-native cpkt C89 symbols:", file=sys.stderr)
        for symbol in unexpected_missing:
            print(f"  {symbol}", file=sys.stderr)
        return 1

    if client_pubsub:
        print(
            "OPC UA cpkt C89 now exposes client PubSub symbols; bind them or document a C-only exclusion:",
            file=sys.stderr,
        )
        for symbol in client_pubsub:
            print(f"  {symbol}", file=sys.stderr)
        return 1

    print(
        f"opcua lua surface ok: {len(symbols) - len(missing)} non-native client/server symbols covered, {len(missing)} native-pointer exclusions documented"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
