use core::ffi::{c_char, CStr};

unsafe extern "C" {
    #[link_name = "kprintf"]
    pub fn kprintf_raw(fmt: *const c_char, ...);

    #[link_name = "arch_pause"]
    fn arch_pause_raw();
}

pub fn kprintf_u64(fmt: &CStr, value: u64) {
    unsafe {
        kprintf_raw(fmt.as_ptr(), value);
    }
}

pub fn arch_pause() {
    unsafe {
        arch_pause_raw();
    }
}
