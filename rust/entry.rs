#![no_std]

use core::ffi::c_void;
use core::hint::spin_loop;

mod ffi;

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        spin_loop();
    }
}

#[no_mangle]
pub extern "C" fn rust_kmain_post_init(cpu_id: u64, _arg: *mut c_void) -> ! {
    kprintf!(c"Arx rust: rust_kmain_post_init entered on cpu %llu\n", cpu_id);

    loop {
        spin_loop();
    }
}
