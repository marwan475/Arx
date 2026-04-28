#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use core::ffi::c_void;

mod allocator;
mod resource;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

#[no_mangle]
pub extern "C" fn __rust_alloc_error_handler(_size: usize, _align: usize) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

#[inline(never)]
#[no_mangle]
pub extern "C" fn rust_entry(cpu_id: u64, _arg: *mut c_void) -> ! {
    resource::kprintf(c"Arx rust: rust_entry entered on cpu %llu\n", cpu_id);

    let mut ids: Vec<u64> = Vec::with_capacity(2);
    ids.push(cpu_id);
    ids.push(cpu_id + 1);
    resource::kprintf(c"Arx rust: Vec online, len=%llu\n", ids.len() as u64);

    loop {
        core::hint::spin_loop();
    }
}
