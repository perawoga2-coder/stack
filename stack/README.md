# STACK for the M-VAVE FM-1

A test build. Eight tracks, each with one sound from a single list; the keys play the selected track.

    base/tools/build.py app --host                         # the simulator (any C compiler)
    app/build/host/stack_host app/tests/basic.txt app/build/host
    base/tools/setup_toolchain.sh                          # JieLi's toolchain and three SDK files (needs their hosts)
    base/tools/build.py app --release "$(cat VERSION)"     # app/build/stack-VERSION.fwsc (the version is the file VERSION)

See `LICENSING.md`, `CREDITS.md` and `app/SOUNDS.md`. `base/` is fm1-base (`BASE_COMMITS.txt`), unchanged.
