#!/bin/sh
# Rocks setup wizard (Docs/Guides/BuildEnvironment.ko.md). Run ./Setup.sh after cloning; Build/Setup.sh lists the options.
exec sh "$(cd "$(dirname "$0")" && pwd)/Build/Setup.sh" "$@"
