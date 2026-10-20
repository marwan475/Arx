"""Uninterrupted boot loop for separating inspection stops from runtime failures."""
import gdb


def run(release=False):
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("hbreak SelfTestManager::Finalize")
    gdb.execute("continue")
    print("BOOT TARGET COMPLETE")
    gdb.execute("detach")
