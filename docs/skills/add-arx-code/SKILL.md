---
name: add-arx-code
description: Follow Arx implementation rules when adding features, fixing bugs, or refactoring first-party code, covering layer dependencies, architecture helpers, readable formatting, and documentation impact.
---

# Add or change Arx code

Apply these rules to first-party implementation changes. Preserve upstream style
when modifying bundled third-party code. The owner's current instructions take
precedence.

## Read the architecture docs for the affected area

Before changing code, read the relevant sections of
[KernelArch.md](../../architecture/KernelArch.md) and the specific guides under
`docs/architecture/` for each layer, subsystem, platform service, or architecture
being changed. For cross-layer work, read the guides for every affected layer.
Use them to establish responsibilities, interfaces, and dependency boundaries,
then check the relevant implementation for current behavior. If no focused guide
exists, use the closest documented section and inspect the affected interfaces.

## Keep layer dependencies downward

Within the layer stack, Request is above Logic, and Logic is above Resource.
An upper layer may access any lower layer: Request may use both Logic and
Resource directly. Passing through every intermediate layer is not required.
Lower layers must not access or depend on layers above them. Keep dependencies
downward through the implementation and its interfaces; introducing a callback
or indirection does not make an upward dependency acceptable.

## Put architecture-specific operations behind arch.h

Use the helpers exposed by [arch.h](../../../arch/arch.h) from shared code.
Read the existing interface and relevant target implementations before adding
a helper. When an operation is missing, extend that interface and place the
architecture-specific implementation under the appropriate target in `arch/`.

Keep CPU instructions, register handling, architecture-specific constants, and
target selection behind the architecture interface. Shared code should express
the operation through a helper rather than embed target-specific logic.

## Treat readable formatting as a completion requirement

Read [kernel.cpp](../../../kernel/kernel.cpp) before writing code and use it as
the authoritative style reference. Formatting is mandatory, not a cosmetic
follow-up. Existing compressed code elsewhere is not a style precedent.

- Use four-space indentation and opening braces on their own lines, following
  `kernel.cpp` for control flow, functions, spacing, and pointer declarations.
- Expand control flow into braced, readable blocks. Put each statement on its
  own line; keep conditions and their bodies separate.
- Use descriptive names, clear intermediate steps, and blank lines between
  logical operations. Break dense expressions into understandable steps when
  that improves readability.
- Be generous with newlines. Separate setup, validation, work, cleanup, and
  return with blank lines where those phases occur. Leave visual space around
  control-flow blocks and between functions; keep closely related declarations
  or statements together. Dense walls of code fail the readability requirement
  even when each statement occupies its own line.
- Avoid compressed, clunky code and clever expressions that make a reader
  unpack several operations at once. Readability takes priority over reducing
  line count.

Follow this layout, adapted from `kernel.cpp`, for declarations, validation,
blank lines, braces, and a final return:

```cpp
static bool BootstrapPostInitTasks(Dispatcher* dispatcher)
{
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: bootstrap post-init tasks skipped (dispatcher null)\n");
        return false;
    }

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();

    if (resourceLayerCaps == nullptr || logicLayerCaps == nullptr)
    {
        kprintf("Arx kernel: bootstrap post-init tasks skipped (missing caps)\n");
        return false;
    }

    // Perform the remaining bootstrap work here.

    return true;
}
```

This is a formatting excerpt, not a complete bootstrap implementation. Keep the
same visual separation in real code without adding comments merely to label
obvious steps.

Before handing off, read every added or changed block against `kernel.cpp` and
expand any code that remains difficult to follow.

## Identify documentation impact

Review the relevant existing guides for claims affected by the change. Report
which descriptions of behavior, interfaces, support, or architecture need an
update, or state that no documentation impact was found.

Edit documentation only when explicitly instructed by the owner. When the owner
invokes [update-kernel-docs](../update-kernel-docs/SKILL.md), follow that skill for
concise maintenance of the affected guides.

## Check the final change

Review the diff for downward layer dependencies, use of architecture helpers,
and compliance with `kernel.cpp` formatting. Correct violations before reporting
the implementation complete, and include the documentation-impact finding in
the handoff.
