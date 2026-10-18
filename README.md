# Arx
Multiarch 64 bit Kernel in development

Targets:

- x86_64 (currently supported)
- aarch64 (experimental)

![Arx](docs/imgs/Arx_Arch.png)

## Documentation

- [Kernel architecture](docs/architecture/KernelArch.md)
- [Platform services](docs/architecture/platform/Platform.md)
- [x86_64 support](docs/architecture/arch/x86_64.md)
- [Dispatcher](docs/architecture/layers/Dispatcher.md)
- [Request layer](docs/architecture/layers/Request.md)
- [Resource layer](docs/architecture/layers/Resource.md)
- [Logic layer](docs/architecture/layers/Logic.md)

## Third Party

- [Limine](https://github.com/limine-bootloader/limine) - Bootloader/protocol used to load the kernel and provide boot info.
- [printf](https://github.com/mpaland/printf) - Small freestanding printf implementation used for kernel logging output.
- [khashl / khashp](https://github.com/attractivechaos/khashl) - Hash-table library by Attractive Chaos, adapted for kernel use; MIT license.
- [uACPI](https://github.com/uACPI/uACPI) - ACPI implementation used for table parsing and ACPI support.
- [Flanterm](https://github.com/Mintsuki/Flanterm) - fast and reasonably complete terminal emulator
- [musl](https://musl.libc.org/) - required userspace runtime for dynamic ELF self-tests; its license is bundled with initramfs.

All third-party components retain their original licenses.
