"""Read-only GDB integration checks against a disposable matching QEMU guest."""
import gdb


def run(release=False):
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("hbreak kmain")
    gdb.execute("continue")
    gdb.execute("source debug/gdb/arx.py")
    gdb.execute("source debug/gdb/arx.py")
    from framework.commands import gather
    from framework.render import present
    from framework.results import Budget, ReadFailure
    from framework.target import Memory
    from views import memory as views

    def command(text, partial=False):
        result, budget = gather(text)
        present(result, budget)
        assert (result.status == "partial") == partial, (text, result.issues)
        return result

    if release:
        try:
            gather("pmm nodes")
        except ReadFailure as error:
            assert "compiled out" in str(error), str(error)
        else:
            raise AssertionError("release unexpectedly exposed allocator providers")
        print("MEMORY TARGET RELEASE PASS")
        gdb.execute("detach")
        return

    state = gdb.lookup_global_symbol("platform").value()
    assert int(state["debugPmm"]["ready"]) == 1
    assert int(state["debugHeap"]["ready"]) == 1
    command("status", partial=True)
    try:
        gather("pmm nodes")
    except ReadFailure as error:
        assert "pre-Dispatcher" in str(error)
    else:
        raise AssertionError("pre-Dispatcher registry unexpectedly available")
    # Early records are readable before publication, without manager calls.
    memory = Memory()
    root = int(state["debugPmm"].address)
    early = views.pmm("nodes", memory,
                      {"root": root, "rootSize": memory.sizeof("arx_pmm_debug_stats_t"),
                       "version": 1, "count": 0, "capacity": 8}, Budget())
    assert early.status == "complete", early.issues
    assert sum(item["path"].startswith("Node ") and " / " not in item["path"]
               for item in early.items) == int(state["numa_node_count"])

    gdb.execute("hbreak SelfTestManager::RunResourceLayerTests")
    gdb.execute("continue")
    command("list")
    command("fixture-c stats")
    command("pmm nodes")
    command("pmm zones --node 0 --zone dma")
    command("pmm orders --node 0 --zone normal")
    command("pmm blocks --zone dma --limit 3", partial=True)
    command("pmm pfn 18446744073709551615", partial=True)
    command("heap caches --class 64")
    command("heap slabs --class 64 --limit 32 --check-bitmap")
    state = gdb.lookup_global_symbol("platform").value()
    slab = state["numa_nodes"][0]["heap"]["caches"][2]["full_slabs"]
    if int(slab):
        address = int(slab.dereference()["objects_array"]) + 1
        selected = command("heap address %#x --class 64 --check-bitmap" % address)
        assert "interior address" in str(selected.items)
    command("dma allocations")

    class Rollback(gdb.Breakpoint):
        def stop(self):
            record = gdb.selected_frame().read_var("record").dereference()
            return int(record["allocation"]["cpuAddress"]) == 0

    rollback = Rollback("DmaManager::Track", type=gdb.BP_HARDWARE_BREAKPOINT)
    gdb.execute("continue")
    manager = gdb.selected_frame().read_var("this").dereference()
    fixture_root = int(manager["DebugStats"].address)
    gdb.execute("finish")
    gdb.execute("finish")
    rollback.delete()

    def fixture_dma(partial=False):
        memory = Memory()
        budget = Budget()
        result = views.dma("allocations", memory,
                           {"root": fixture_root, "rootSize": memory.sizeof("DmaDebugStats"),
                            "version": 1, "count": 0, "capacity": 0}, budget)
        present(result, budget)
        assert (result.status == "partial") == partial, result.issues
        return result

    retained = fixture_dma()
    assert "cleanup-only failed allocation" in str(retained.items)
    assert "8192 reserved backing bytes" in str(retained.items)
    assert "satisfies constraints=False" in str(retained.items)

    class Releasing(gdb.Breakpoint):
        def stop(self):
            context = gdb.selected_frame().read_var("context")
            mock = context.cast(gdb.lookup_type("DmaMock").pointer()).dereference()
            return int(mock["releasing"]) != 0

    releasing = Releasing("MockUnmap", type=gdb.BP_HARDWARE_BREAKPOINT)
    gdb.execute("continue")
    transitional = fixture_dma(partial=True)
    assert "phase=unmapping" in str(transitional.items)
    releasing.delete()
    gdb.execute("hbreak SelfTestManager::RunLogicLayerTests")
    gdb.execute("continue")
    command("pmm nodes")
    command("dma allocations")
    command("fixture-c stats")
    print("MEMORY TARGET DEBUG PASS")
    gdb.execute("detach")
