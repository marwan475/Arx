#![no_std]

use core::ffi::c_void;

mod ffi;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        ffi::arch_pause();
    }
}

#[inline(never)]
#[no_mangle]
pub extern "C" fn rust_entry(cpu_id: u64, _arg: *mut c_void) -> ! {
    ffi::kprintf_u64(c"Arx rust: rust_entry entered on cpu %llu\n", cpu_id);

    loop {
        ffi::arch_pause();
    }
}
