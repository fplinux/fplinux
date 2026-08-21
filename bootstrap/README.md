# Shared pre-Linux components

`bootstrap/` owns freestanding components reused before Linux starts. They have
no Linux userspace dependency and do not contain phone-specific addresses,
register setup, panel sequences, or loader policy.

A target owns hardware initialization, display presentation, board diagnostics,
and transfer to its Linux image. Shared bootstrap code may expose bounded
callbacks for those target-owned operations, but must not take over the board
contract.

Use this directory only for behavior already shared by current targets. See
[Shared host and runtime stack](../common/README.md) for post-kernel code.
