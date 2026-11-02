# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

"""`kernel reboot cold` on the native_sim build of app_diag.

native_sim reboots by re-running itself with the same arguments, so the
store's backing file outlives the reboot. After it the app is unmounted
again, and mounting finds the store formatted before the reboot rather than
formatting a blank partition.
"""

from twister_harness import DeviceAdapter, Shell


def _has(lines: list[str], text: str) -> bool:
    return any(text in line for line in lines)


def test_reboot_keeps_store(dut: DeviceAdapter, shell: Shell):
    lines = shell.exec_command('blob_db format yes')
    assert _has(lines, 'formatted and mounted'), lines

    # Not exec_command: the shell that would print the next prompt is the one
    # being replaced, so wait for the new boot instead.
    dut.write(b'kernel reboot cold\n')
    dut.readlines_until(regex='kvdb diag: not mounted', timeout=10.0)
    assert shell.wait_for_prompt(), 'no prompt after reboot'

    lines = shell.exec_command('blob_db info')
    assert _has(lines, 'blob_db not mounted'), lines

    lines = shell.exec_command('blob_db mount')
    assert _has(lines, 'mounted, '), lines
    # The mount logs UBI formatting a partition it finds blank; finding the
    # store from before the reboot, it only attaches.
    assert not _has(lines, 'formatted'), lines
    assert not _has(lines, 'holds no UBI device'), lines

    lines = shell.exec_command('blob_db stats')
    assert _has(lines, '1 live ids'), lines
