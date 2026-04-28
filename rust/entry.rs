#![no_std]

use core::ffi::c_void;

mod resource;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

#[inline(never)]
#[no_mangle]
pub extern "C" fn rust_entry(cpu_id: u64, _arg: *mut c_void) -> ! {
    resource::kprintf(c"Arx rust: rust_entry entered on cpu %llu\n", cpu_id);

    loop {
        core::hint::spin_loop();
    }
}
