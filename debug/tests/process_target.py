"""Live process debug inspection through existing disposable kernel fixtures."""
from pathlib import Path
import gdb


def run(release=False):
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    first = gdb.Breakpoint("kmain", type=gdb.BP_HARDWARE_BREAKPOINT)
    gdb.execute("continue")
    first.delete()
    gdb.execute("source debug/gdb/arx.py")
    from framework.commands import gather
    from framework.render import present
    from framework.results import ReadFailure

    def command(text, partial=False):
        result, budget = gather(text)
        present(result, budget)
        if partial is not None:
            assert (result.status == "partial") == partial, (text, result.issues)
        return result

    if release:
        try:
            gather("process tree")
        except ReadFailure as error:
            assert "compiled out" in str(error)
        else:
            raise AssertionError("release exposed process tracking")
        print("PROCESS TARGET RELEASE PASS")
        gdb.execute("detach")
        return

    def source_stop(path, marker):
        lines = Path(path).read_text().splitlines()
        line = next(index + 1 for index, text in enumerate(lines) if marker in text)
        point = gdb.Breakpoint("%s:%d" % (path, line), type=gdb.BP_HARDWARE_BREAKPOINT)
        gdb.execute("continue")
        point.delete()

    source_stop("kernel/selftests/resource/ProcessManagerTests.cpp", 't.Equal(child->debug.parentGeneration, oldGeneration, "capture')
    child = gdb.selected_frame().read_var("child").dereference()
    pid = int(child["id"])
    command("process tree")
    command("process details --pid %d" % pid)
    command("process credentials --pid %d" % pid)
    source_stop("kernel/selftests/resource/ProcessManagerTests.cpp", 't.Equal(child->debug.parentGeneration, oldGeneration, "refresh')
    command("process tree", partial=True)
    source_stop("kernel/selftests/resource/ProcessManagerTests.cpp", 't.Check(ready, "attach threads with private')
    second = gdb.selected_frame().read_var("second").dereference()
    command("task files --tid %d" % int(second["id"]))
    command("task details --tid %d" % int(second["id"]))
    source_stop("kernel/selftests/logic/CredentialManagerTests.cpp", 't.Equal(m->DebugStats.identities.refusals')
    command("credentials stats")
    command("lifecycle stats")
    source_stop("kernel/selftests/request/ProcessRequestManagerTests.cpp", 'int32_t reportedPid = -1;')
    child_pid = int(gdb.selected_frame().read_var("deferredChild"))
    deferred = gdb.selected_frame().read_var("deferredTask").dereference()
    assert int(deferred["dispatchUsers"]) > 0
    detail = command("process details --pid %d" % child_pid)
    assert "exited / retained" in str(detail.items)
    assert "notified=False" in str(detail.items)
    command("task details --tid %d" % int(deferred["id"]))
    source_stop("kernel/selftests/request/ProcessRequestManagerTests.cpp", 'passed &= expect(child->affinityMask == inheritedAffinity')
    child = gdb.selected_frame().read_var("child").dereference()
    files = command("task files --tid %d" % int(child["id"]))
    assert "Open file" in str(files.items)
    command("process tree", partial=None)
    final = gdb.Breakpoint("SelfTestManager::Finalize", type=gdb.BP_HARDWARE_BREAKPOINT)
    gdb.execute("continue")
    final.delete()
    command("lifecycle stats")
    command("process stats", partial=None)
    command("task stats")
    # Completed clone/exec/wait behavior is supplied by the existing syscall
    # lifecycle fixtures, not synthesized by the display implementation.
    state = gdb.lookup_global_symbol("platform").value()
    dispatcher = state["dispacher"].cast(gdb.lookup_type("Dispatcher").pointer()).dereference()
    # Find the lifecycle record in the registry without calling factory getters.
    registry = dispatcher["debugManager"]
    found = False
    for index in range(int(registry["reserved"])):
        record = registry["records"][index]
        if record["name"].string() == "lifecycle":
            stats = record["address"].cast(gdb.lookup_type("LifecycleDebugStats").pointer()).dereference()
            assert int(stats["clone"]["successes"]) > 0
            assert int(stats["exec"]["successes"]) > 0
            assert int(stats["waitsObserved"]) > 0
            found = True
    assert found
    print("PROCESS TARGET DEBUG PASS")
    gdb.execute("detach")
