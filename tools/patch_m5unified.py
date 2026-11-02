"""Raise M5Unified's speaker task stack before it is compiled.

M5Unified sizes the speaker task as ``1280 + dma_buf_len * 4`` bytes, then
``spk_task`` allocates ``dma_buf_len * 4`` of that with ``alloca()`` and uses
a ~480 byte frame. That leaves only ~800 bytes for call depth and interrupt
frames. On the AtomS3R + Atomic Echo Base path an interrupt frame reached the
task canary and the firmware panicked with:

    Guru Meditation Error: Core 1 panic'ed (Unhandled debug exception).
    Debug exception reason: Stack canary watchpoint triggered (spk_task)

The library lives in ``.pio/libdeps``, so the fix is applied to the installed
copy at project load time. It is idempotent and does not touch the file when
it already has the larger stack.
"""

import os

Import("env")

NEEDLE = "size_t stack_size = 1280 + "
REPLACEMENT = "size_t stack_size = 4096 + "


def patch_speaker_stack(path):
    try:
        with open(path, "r", encoding="utf-8", newline="") as handle:
            text = handle.read()
    except OSError as exc:
        print("patch_m5unified: cannot read %s: %s" % (path, exc))
        return

    if REPLACEMENT in text or NEEDLE not in text:
        return

    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text.replace(NEEDLE, REPLACEMENT, 1))
    print("patch_m5unified: raised speaker task stack in %s" % path)


patch_speaker_stack(
    os.path.join(
        env.subst("$PROJECT_LIBDEPS_DIR"),
        env.subst("$PIOENV"),
        "M5Unified",
        "src",
        "utility",
        "Speaker_Class.inl",
    )
)
